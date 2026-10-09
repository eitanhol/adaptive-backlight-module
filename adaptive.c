// Copyright 2025-2026 eitanhol
// SPDX-License-Identifier: GPL-2.0-or-later
/* Adaptive backlight - QMK community module.
 *
 * Board-agnostic: everything keyboard-specific comes from QMK at compile
 * time (g_led_config, MATRIX_ROWS/COLS, AM_LED_COUNT) and is
 * reported to the app at run time over VIA raw HID (channel 0x42).
 *
 * Attach it to any board by adding keymaps/<name>/keymap.json with
 *   { "modules": ["adaptive"] }
 * next to that board's normal keymap.c (tools/add_board.py does this).
 *
 * Hooks used: process_record_<module>, housekeeping_task_<module>,
 * keyboard_post_init_<module>, and the weak via_custom_value_command_kb
 * from quantum/via.c (Keychron's firmware leaves that one unclaimed).
 */

#include QMK_KEYBOARD_H
#include "via.h"
#include "adaptive_params.h"
#include "adaptive_api.h"

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 0, 0);


/* ------------------------------------------------------------------
 * SOCD cleaner - Last Input Priority with reactivation
 *
 * Affects ONLY the two keycodes named below. Every other key on the
 * board returns early on the first check and behaves completely
 * normally.
 *
 *   Hold A, press D          -> A released, D active
 *   Release D, A still held  -> A becomes active again
 *   Release A, D still held  -> D stays active
 *
 * This deliberately does NOT register/unregister the key you actually
 * pressed - QMK still does that itself. We only ever add or remove the
 * OPPOSING key. Handling both is what caused the dead input on release.
 *
 * Reactivation only happens for a key this filter actually suppressed.
 * Without that check, releasing the FIRST key of a typing rollover
 * ("ad" -> release A while D is still down) called register_code(D) on
 * a key that was never released; QMK forces a release+press in that
 * case so the host sees a fresh keystroke, which typed "downloadd".
 * Game behaviour is unchanged: in that row D was already active and
 * simply stays active.
 * ------------------------------------------------------------------ */

#define SOCD_KEY_1 KC_A
#define SOCD_KEY_2 KC_D

static bool socd_1_held = false, socd_1_suppressed = false;
static bool socd_2_held = false, socd_2_suppressed = false;

/* Armed/disarmed. The filter runs only while armed, so it never touches
 * normal typing when off. The state is kept in bit0 of the EEPROM block's
 * spare word, so saved presets are not moved. It is toggled by the app
 * (value id 0x9D) or by a key chord recorded from the keyboard (0x9E). */
static bool socd_armed = false;   /* persisted */
static bool socd_on    = false;   /* the filter is active */

static void socd_update(void) {
    bool on = socd_armed;
    if (!on && socd_on) {
        /* going quiet mid-hold: give a suppressed key back to the host so
         * nothing is left in a state QMK does not know about */
        if (socd_1_held && socd_1_suppressed) register_code(SOCD_KEY_1);
        if (socd_2_held && socd_2_suppressed) register_code(SOCD_KEY_2);
        socd_1_suppressed = socd_2_suppressed = false;
    }
    socd_on = on;
}

static void adaptive_store(void); /* below: writes the EEPROM block */
static void socd_set_armed(bool armed) {
    socd_armed = armed;
    socd_update();
    adaptive_store();
}

/* ---- toggle chord: up to 4 keycodes that must all be down ----
 * Recording (started by the app): every key pressed is swallowed and
 * appended; the first key's release ends it and saves. Triggering: the
 * press that completes the chord toggles SOCD and is swallowed (its
 * release too), then nothing fires again until a chord key is released. */
static uint16_t socd_chord[ADAPTIVE_SOCD_CHORD_MAX];   /* 0 = unused */
static uint8_t  socd_chord_n = 0;
static uint8_t  socd_rec_state = 0;                     /* 0 idle, 1 recording, 2 just recorded */
static uint16_t socd_rec_first = 0;
static uint16_t socd_rec_down[8];                       /* keys pressed during recording, so only their releases are swallowed */
static uint8_t  socd_rec_down_n = 0;
static uint16_t socd_down[8];                           /* keys currently down (for chord matching) */
static uint8_t  socd_down_n = 0;
static bool     socd_chord_fired = false;
static uint16_t socd_swallow_release = 0;               /* the chord's completing key: eat its release */

static void socd_chord_load(void);
static void socd_chord_save(void);

static bool socd_list_has(const uint16_t *l, uint8_t n, uint16_t kc) {
    for (uint8_t i = 0; i < n; i++) if (l[i] == kc) return true;
    return false;
}
static void socd_list_add(uint16_t *l, uint8_t *n, uint8_t max, uint16_t kc) {
    if (*n < max && !socd_list_has(l, *n, kc)) l[(*n)++] = kc;
}
static void socd_list_del(uint16_t *l, uint8_t *n, uint16_t kc) {
    for (uint8_t i = 0; i < *n; i++) if (l[i] == kc) { l[i] = l[--(*n)]; return; }
}

/* returns true when the event was consumed */
static bool socd_chord_process(uint16_t keycode, keyrecord_t *record) {
    if (socd_rec_state == 1) {
        if (record->event.pressed) {
            if (socd_rec_first == 0) socd_rec_first = keycode;
            socd_list_add(socd_rec_down, &socd_rec_down_n, 8, keycode);
            if (socd_chord_n < ADAPTIVE_SOCD_CHORD_MAX && !socd_list_has(socd_chord, socd_chord_n, keycode)) {
                socd_chord[socd_chord_n++] = keycode;
            }
            return true;
        }
        if (socd_list_has(socd_rec_down, socd_rec_down_n, keycode)) {
            socd_list_del(socd_rec_down, &socd_rec_down_n, keycode);
            if (keycode == socd_rec_first) {       /* first key up: done */
                socd_rec_state = 2;
                socd_chord_save();
            }
            return true;
        }
        return false;                              /* was down before recording started */
    }
    if (socd_rec_state == 2 && socd_rec_down_n) {  /* remaining recorded keys still coming up */
        if (!record->event.pressed && socd_list_has(socd_rec_down, socd_rec_down_n, keycode)) {
            socd_list_del(socd_rec_down, &socd_rec_down_n, keycode);
            return true;
        }
    }

    if (record->event.pressed) {
        socd_list_add(socd_down, &socd_down_n, 8, keycode);
        if (socd_chord_n && !socd_chord_fired && socd_list_has(socd_chord, socd_chord_n, keycode)) {
            bool all = true;
            for (uint8_t i = 0; i < socd_chord_n; i++) if (!socd_list_has(socd_down, socd_down_n, socd_chord[i])) all = false;
            if (all) {
                socd_chord_fired      = true;
                socd_swallow_release  = keycode;
                socd_set_armed(!socd_armed);
                return true;
            }
        }
    } else {
        socd_list_del(socd_down, &socd_down_n, keycode);
        if (socd_list_has(socd_chord, socd_chord_n, keycode)) socd_chord_fired = false;
        if (keycode == socd_swallow_release) { socd_swallow_release = 0; return true; }
    }
    return false;
}

static void socd_chord_begin(void) {
    socd_rec_state = 1; socd_rec_first = 0; socd_rec_down_n = 0; socd_chord_n = 0;
    memset(socd_chord, 0, sizeof(socd_chord));
}
static void socd_chord_clear(void) {
    socd_rec_state = 0; socd_chord_n = 0;
    memset(socd_chord, 0, sizeof(socd_chord));
    socd_chord_save();
}

static void socd_filter(uint16_t keycode, keyrecord_t *record) {
    uint16_t other;
    bool    *self_held, *self_supp, *other_held, *other_supp;

    if (keycode == SOCD_KEY_1) {
        other      = SOCD_KEY_2;
        self_held  = &socd_1_held;
        self_supp  = &socd_1_suppressed;
        other_held = &socd_2_held;
        other_supp = &socd_2_suppressed;
    } else if (keycode == SOCD_KEY_2) {
        other      = SOCD_KEY_1;
        self_held  = &socd_2_held;
        self_supp  = &socd_2_suppressed;
        other_held = &socd_1_held;
        other_supp = &socd_1_suppressed;
    } else {
        return; /* any other key: do nothing at all */
    }

    if (record->event.pressed) {
        *self_held = true;
        *self_supp = false; /* QMK is registering it right now */
        if (*other_held && !*other_supp) {
            unregister_code(other); /* newest press wins */
            *other_supp = true;
        }
    } else {
        *self_held = false;
        *self_supp = false; /* QMK unregisters it (no-op if already off) */
        if (*other_held && *other_supp) {
            register_code(other); /* reactivate the key we turned off */
            *other_supp = false;
        }
    }
}

/* ------------------------------------------------------------------
 * Runtime parameters
 *
 * Live-settable over USB via the VIA raw HID endpoint, so tuning no
 * longer needs a recompile+reflash cycle. Values live in RAM and can
 * be committed to EEPROM with the save command.
 * ------------------------------------------------------------------ */

#define ADAPTIVE_EE_MAGIC 0xAD04 /* bumped: slot stride grew to 40 params */

/* defined in adaptive_effect.inc / later in this file; the state
 * command below reads them */
extern uint8_t  adaptive_blend_level;
extern bool     adaptive_keys_held;
extern uint8_t  adaptive_held_count;
extern uint8_t  adaptive_held_leds[];
extern uint8_t  adaptive_kps_energy;

/* defined in the adaptive section below; the VIA hit command calls it */
static void adaptive_inject_hit(uint8_t led, uint8_t pressed);
/* defined below; the long-idle preview command rewinds it */
extern uint32_t last_input_time;

/* written by the effect each frame; how far into long-term idle we are */
uint8_t adaptive_lt_level = 0;

/* Idle-animation epoch. The wave/flicker phase is computed from
 * (am_timer - epoch), so setting epoch = now restarts the idle
 * animation from phase zero. The app triggers this when Live update
 * turns on, and the state report sends the epoch-relative timer, so
 * board and sim restart from the same instant and the same phase.
 * Referenced by adaptive_effect.inc. */
uint32_t adaptive_wave_epoch = 0;

/* Startup-animation epoch: 0 at boot so the animation plays once at
 * power-on; the app can set it to "now" to replay for previewing. */
uint32_t adaptive_su_epoch = 0;

uint16_t adaptive_params[AP_COUNT];

static const uint16_t adaptive_defaults[AP_COUNT] = {
    260,  /* AP_ATTACK_MS     */
    900,  /* AP_RELEASE_MS    */
    260,  /* AP_HOLD_MS       */
    5,    /* AP_REACT_FALLOFF */
    7,    /* AP_HELD_FALLOFF  */
    10,   /* AP_WAVE_DIV      */
    7,    /* AP_FLICK_DIV     */
    110,  /* AP_FLICK_DEPTH   */
    29,   /* AP_FLICK_SPREAD  */
    0,    /* AP_WAVE_DIR: horizontal      */
    0,    /* AP_IDLE_FLOOR: none          */
    0,    /* AP_STRIKE_MODE: glow         */
    0,    /* AP_STRIKE_DUR: follow speed  */
    0,    /* AP_KPS_GAIN: off             */
    0,    /* AP_HELD_LEVEL: 0 = full      */
    0,    /* AP_IDLE_STYLE: wave          */
    0,    /* AP_BLEND_DEPTH: 0 = legacy full crossfade */
    0,    /* AP_STARTUP_STYLE: none       */
    0,    /* AP_LT_STYLE: disabled        */
    0,    /* AP_LT_MINUTES: 0 = default 5 */
    0,    /* AP_LT_LEVEL: 0 = full        */
    10,   /* AP_LT_WAVE_DIV               */
    0,    /* AP_LT_WAVE_DIR: horizontal   */
    110,  /* AP_LT_FLICK_DEPTH            */
    7,    /* AP_LT_FLICK_DIV              */
    29,   /* AP_LT_FLICK_SPREAD           */
    0,    /* AP_HELD_MODE: halo           */
    0,    /* AP_HELD_DUR: 0 = 1500 ms     */
    0,    /* AP_HELD_DELAY: immediate     */
    0,    /* AP_BLEND_MODE: whole board   */
    0,    /* AP_WAVE_WIDTH: 0 = normal    */
    0,    /* AP_PULSE_HIGH: breathing off */
    0,    /* AP_PULSE_LOW                 */
    10,   /* AP_PULSE_DIV                 */
    0,    /* AP_LT_WAVE_WIDTH: 0 = normal */
    0     /* AP_STARTUP_DUR: 0 = 2000 ms  */
};

/* 20 padded param slots per preset so adding params never resizes the
 * datablock again. Must total EECONFIG_USER_DATA_SIZE (168) exactly. */
#define ADAPTIVE_SLOT_PAD 40
typedef struct {
    uint16_t magic;
    uint16_t active;
    uint16_t p[ADAPTIVE_PRESETS][ADAPTIVE_SLOT_PAD];
    uint16_t count; /* presets in use, 1..ADAPTIVE_PRESETS. This was a zeroed
                     * spare word, so boards saved before it existed read 0
                     * and are treated as "one preset" without a wipe. */
    uint16_t spare;
} adaptive_ee_t;
_Static_assert(sizeof(adaptive_ee_t) == EECONFIG_USER_DATA_SIZE,
               "preset block must fill the eeconfig user datablock");

/* RAM master copy of all slots; EEPROM is its backup. */
static adaptive_ee_t adaptive_ee;

#define ADAPTIVE_SPARE_SOCD_ARMED 0x0001 /* bit0 of adaptive_ee.spare */
static void adaptive_store(void) {
    adaptive_ee.spare = (adaptive_ee.spare & ~ADAPTIVE_SPARE_SOCD_ARMED) | (socd_armed ? ADAPTIVE_SPARE_SOCD_ARMED : 0);
    eeconfig_update_user_datablock(&adaptive_ee);
}
/* the chord lives in the padding words of slot 0 (p[0][AP_COUNT..]):
 * saves copy only AP_COUNT words into a slot, and slot deletes shift only
 * AP_COUNT words, so these never move */
_Static_assert(AP_COUNT + ADAPTIVE_SOCD_CHORD_MAX <= ADAPTIVE_SLOT_PAD, "no room for the chord in the slot padding");
static void socd_chord_load(void) {
    socd_chord_n = 0;
    for (uint8_t i = 0; i < ADAPTIVE_SOCD_CHORD_MAX; i++) {
        uint16_t kc = adaptive_ee.p[0][AP_COUNT + i];
        if (kc == 0 || kc == 0xFFFF) break;
        socd_chord[socd_chord_n++] = kc;
    }
}
static void socd_chord_save(void) {
    for (uint8_t i = 0; i < ADAPTIVE_SOCD_CHORD_MAX; i++) adaptive_ee.p[0][AP_COUNT + i] = (i < socd_chord_n) ? socd_chord[i] : 0;
    eeconfig_update_user_datablock(&adaptive_ee);
}

/* Divisors are used as denominators in the effect - a zero would
 * fault the board and leave it needing a reflash to recover. */
static uint16_t adaptive_clamp(uint8_t id, uint16_t v) {
    switch (id) {
        case AP_ATTACK_MS:
        case AP_RELEASE_MS:
            return (v < 1) ? 1 : v;
        case AP_WAVE_DIV:
        case AP_FLICK_DIV:
        case AP_REACT_FALLOFF:
        case AP_HELD_FALLOFF:
            return (v < 1) ? 1 : v;
        case AP_FLICK_DEPTH:
        case AP_BLEND_DEPTH:
            return (v > 255) ? 255 : v;
        case AP_IDLE_STYLE:
            return (v > 4) ? 0 : v;
        case AP_STRIKE_MODE:
            return (v > 4) ? 0 : v;
        case AP_STARTUP_STYLE:
            return (v > 10) ? 0 : v;
        case AP_LT_STYLE:
            return (v > 5) ? 0 : v;
        case AP_LT_MINUTES:
            return (v > 120) ? 120 : v;
        case AP_LT_LEVEL:
        case AP_LT_FLICK_DEPTH:
        case AP_LT_FLICK_SPREAD:
            return (v > 255) ? 255 : v;
        case AP_LT_WAVE_DIV:
        case AP_LT_FLICK_DIV:
            return (v < 1) ? 1 : (v > 64) ? 64 : v;
        case AP_LT_WAVE_DIR:
            return (v > 3) ? 0 : v;
        case AP_HELD_MODE:
            return (v > 2) ? 0 : v;
        case AP_HELD_DUR:
            return (v > 10000) ? 10000 : v;
        case AP_HELD_DELAY:
            return (v > 2000) ? 2000 : v;
        case AP_BLEND_MODE:
            return (v > 1) ? 0 : v;
        case AP_WAVE_WIDTH:
        case AP_LT_WAVE_WIDTH:
            return (v && v < 4) ? 4 : (v > 64) ? 64 : v;
        case AP_PULSE_HIGH:
        case AP_PULSE_LOW:
            return (v > 255) ? 255 : v;
        case AP_PULSE_DIV:
            return (v < 1) ? 1 : (v > 64) ? 64 : v;
        case AP_STARTUP_DUR:
            return (v && v < 500) ? 500 : (v > 6000) ? 6000 : v;
        default:
            return v;
    }
}

static void adaptive_apply_slot(uint8_t s) {
    for (uint8_t i = 0; i < AP_COUNT; i++) {
        adaptive_params[i] = adaptive_clamp(i, adaptive_ee.p[s][i]);
    }
}

static void adaptive_load(void) {
    eeconfig_read_user_datablock(&adaptive_ee);
    if (adaptive_ee.magic != ADAPTIVE_EE_MAGIC || adaptive_ee.active >= ADAPTIVE_PRESETS) {
        memset(&adaptive_ee, 0, sizeof(adaptive_ee));
        adaptive_ee.magic  = ADAPTIVE_EE_MAGIC;
        adaptive_ee.active = 0;
        adaptive_ee.count  = 1;
        for (uint8_t s = 0; s < ADAPTIVE_PRESETS; s++) {
            memcpy(adaptive_ee.p[s], adaptive_defaults, sizeof(adaptive_defaults));
        }
    }
    /* legacy blocks (count 0) show only P1; extra slot data stays on the
     * chip, just hidden, so nothing anyone saved is destroyed */
    if (adaptive_ee.count < 1 || adaptive_ee.count > ADAPTIVE_PRESETS) adaptive_ee.count = 1;
    if (adaptive_ee.active >= adaptive_ee.count) adaptive_ee.active = 0;
    adaptive_apply_slot((uint8_t)adaptive_ee.active);
    socd_armed = (adaptive_ee.spare & ADAPTIVE_SPARE_SOCD_ARMED) != 0;
    socd_update();
    socd_chord_load();
}

/* Save = write the live values into the ACTIVE preset slot. */
static void adaptive_save(void) {
    memcpy(adaptive_ee.p[adaptive_ee.active], adaptive_params, sizeof(adaptive_params));
    eeconfig_update_user_datablock(&adaptive_ee);
}

static bool adaptive_dirty(void) {
    return memcmp(adaptive_params, adaptive_ee.p[adaptive_ee.active],
                  sizeof(adaptive_params)) != 0;
}

static void adaptive_switch_slot(uint8_t s) {
    if (s >= adaptive_ee.count) return;
    adaptive_ee.active = s;
    adaptive_apply_slot(s);
    eeconfig_update_user_datablock(&adaptive_ee); /* survive a power cycle */
    adaptive_wave_epoch = am_timer;            /* visible switch feedback */
}

/* New preset = a copy of whatever is live right now, made active. */
static void adaptive_slot_add(void) {
    if (adaptive_ee.count >= ADAPTIVE_PRESETS) return;
    uint8_t n = (uint8_t)adaptive_ee.count;
    memcpy(adaptive_ee.p[n], adaptive_params, sizeof(adaptive_params));
    adaptive_ee.count  = n + 1;
    adaptive_ee.active = n;
    eeconfig_update_user_datablock(&adaptive_ee);
    adaptive_wave_epoch = am_timer;
}

/* Delete preset s; the ones after it shift down. Always keeps one. */
static void adaptive_slot_del(uint8_t s) {
    if (adaptive_ee.count <= 1 || s >= adaptive_ee.count) return;
    bool reload = (adaptive_ee.active == s);
    for (uint8_t i = s; i + 1 < adaptive_ee.count; i++) {
        memcpy(adaptive_ee.p[i], adaptive_ee.p[i + 1], sizeof(adaptive_params)); /* params only; padding holds the chord */
    }
    adaptive_ee.count--;
    if (adaptive_ee.active > s) adaptive_ee.active--;
    if (adaptive_ee.active >= adaptive_ee.count) adaptive_ee.active = adaptive_ee.count - 1;
    if (reload) adaptive_apply_slot((uint8_t)adaptive_ee.active);
    eeconfig_update_user_datablock(&adaptive_ee);
    adaptive_wave_epoch = am_timer;
}

static void adaptive_revert(void) {
    adaptive_apply_slot((uint8_t)adaptive_ee.active);
    adaptive_wave_epoch = am_timer;
}

void via_custom_value_command_kb(uint8_t *data, uint8_t length) {
    /* data = [ command_id, channel_id, value_id, value_data... ] */
    uint8_t *command_id = &data[0];
    uint8_t *channel_id = &data[1];
    uint8_t *value_id   = &data[2];
    uint8_t *value_data = &data[3];

    if (*channel_id != ADAPTIVE_VIA_CHANNEL) {
        *command_id = id_unhandled;
        return;
    }

    switch (*command_id) {
        case id_custom_set_value:
            if (*value_id < AP_COUNT) {
                uint16_t v = ((uint16_t)value_data[0] << 8) | value_data[1];
                adaptive_params[*value_id] = adaptive_clamp(*value_id, v);
            } else if (*value_id == ADAPTIVE_VID_SPEED) {
                am_set_speed_noeeprom(value_data[1]);
            } else if (*value_id == ADAPTIVE_VID_VAL) {
                am_set_val_noeeprom(value_data[1]);
            } else if (*value_id == ADAPTIVE_VID_HUE) {
                am_set_hue_noeeprom(value_data[1]); /* no-op on white boards */
            } else if (*value_id == ADAPTIVE_VID_SAT) {
                am_set_sat_noeeprom(value_data[1]);
            } else if (*value_id == ADAPTIVE_VID_HIT) {
                /* synthetic keypress: mouse clicks on the simulator
                 * light the physical board like a real key */
                adaptive_inject_hit(value_data[0], value_data[1]);
            } else if (*value_id == ADAPTIVE_VID_RESTART) {
                adaptive_wave_epoch = am_timer;
            } else if (*value_id == ADAPTIVE_VID_STARTUP) {
                adaptive_su_epoch = am_timer;
            } else if (*value_id == ADAPTIVE_VID_LT_PREVIEW) {
                /* rewind the idle clock past the long-idle threshold so
                 * the long-idle look engages immediately; typing again
                 * restores normal timing */
                uint32_t mins = adaptive_params[AP_LT_MINUTES] ? adaptive_params[AP_LT_MINUTES] : 5;
                last_input_time = timer_read32() - (mins * 60000u + 4200u);
            } else if (*value_id == ADAPTIVE_VID_SLOT) {
                adaptive_switch_slot(value_data[0]);
            } else if (*value_id == ADAPTIVE_VID_SLOT_ADD) {
                adaptive_slot_add();
            } else if (*value_id == ADAPTIVE_VID_SLOT_DEL) {
                adaptive_slot_del(value_data[0]);
            } else if (*value_id == ADAPTIVE_VID_REVERT) {
                adaptive_revert();
            } else if (*value_id == ADAPTIVE_VID_SOCD) {
                socd_set_armed(value_data[0] != 0);
            } else if (*value_id == ADAPTIVE_VID_SOCD_CHORD) {
                if (value_data[0] == 1) socd_chord_begin();
                else if (value_data[0] == 2) socd_chord_clear();
                else { if (socd_rec_state == 1) socd_chord_load(); socd_rec_state = 0; socd_rec_down_n = 0; } /* cancel: back to the saved chord */
            } else if (*value_id == ADAPTIVE_VID_REBOOT) {
                /* jump straight to the DFU bootloader so the flasher can
                 * grab it with no keypress. Same call QK_BOOT makes. */
                reset_keyboard();
            }
            break;

        case id_custom_get_value:
            if (*value_id < AP_COUNT) {
                value_data[0] = adaptive_params[*value_id] >> 8;
                value_data[1] = adaptive_params[*value_id] & 0xFF;
            } else if (*value_id == ADAPTIVE_VID_SPEED) {
                value_data[0] = 0;
                value_data[1] = am_get_speed();
            } else if (*value_id == ADAPTIVE_VID_VAL) {
                value_data[0] = 0;
                value_data[1] = am_get_val();
            } else if (*value_id == ADAPTIVE_VID_HUE) {
                value_data[0] = 0;
                value_data[1] = am_get_hue();
            } else if (*value_id == ADAPTIVE_VID_SAT) {
                value_data[0] = 0;
                value_data[1] = am_get_sat();
            } else if (*value_id == ADAPTIVE_VID_SLOT) {
                value_data[0] = (uint8_t)adaptive_ee.active;
                value_data[1] = adaptive_dirty() ? 1 : 0;
                value_data[2] = adaptive_lt_level;
                value_data[3] = (uint8_t)adaptive_ee.count;
                value_data[4] = ADAPTIVE_PRESETS;
                value_data[5] = socd_on ? 1 : 0; /* so the app mirrors key toggles */
            } else if (*value_id == ADAPTIVE_VID_SOCD) {
                value_data[0] = socd_armed ? 1 : 0;
                value_data[1] = 0;
                value_data[2] = 1; /* chord support */
            } else if (*value_id == ADAPTIVE_VID_SOCD_CHORD) {
                value_data[0] = socd_rec_state;
                value_data[1] = socd_chord_n;
                for (uint8_t i = 0; i < ADAPTIVE_SOCD_CHORD_MAX; i++) {
                    value_data[2 + i * 2] = socd_chord[i] >> 8;
                    value_data[3 + i * 2] = socd_chord[i] & 0xFF;
                }
                if (socd_rec_state == 2) socd_rec_state = 0; /* "just recorded" is reported once */
            } else if (*value_id == ADAPTIVE_VID_GEOM_INFO) {
                /* board self-description: the app builds its LED table
                 * and key-to-LED map from these instead of shipping a
                 * copy of g_led_config per keyboard */
                value_data[0] = ADAPTIVE_GEOM_VERSION;
                value_data[1] = AM_LED_COUNT;
                value_data[2] = MATRIX_ROWS;
                value_data[3] = MATRIX_COLS;
                value_data[4] = AM_RGB; /* bit0: colour-capable */
                value_data[5] = AP_COUNT;
                value_data[6] = ADAPTIVE_PRESETS;
                /* which base layer is live (Keychron's Mac/Win switch
                 * picks 0 or 2), so the app draws the right legends */
                value_data[7] = get_highest_layer(default_layer_state);
            } else if (*value_id == ADAPTIVE_VID_GEOM_POINTS) {
                uint8_t first = value_data[0];
                uint8_t n     = 0;
                for (uint8_t i = 0; i < ADAPTIVE_GEOM_POINTS_PER_PKT; i++) {
                    uint16_t li = (uint16_t)first + i;
                    if (li >= AM_LED_COUNT) break;
                    value_data[2 + i * 2] = g_led_config.point[li].x;
                    value_data[3 + i * 2] = g_led_config.point[li].y;
                    n++;
                }
                value_data[1] = n;
            } else if (*value_id == ADAPTIVE_VID_GEOM_MATRIX) {
                uint8_t row   = value_data[0];
                uint8_t first = value_data[1];
                uint8_t n     = 0;
                if (row < MATRIX_ROWS) {
                    for (uint8_t i = 0; i < ADAPTIVE_GEOM_COLS_PER_PKT; i++) {
                        uint16_t c = (uint16_t)first + i;
                        if (c >= MATRIX_COLS) break;
                        value_data[3 + i] = g_led_config.matrix_co[row][c];
                        n++;
                    }
                }
                value_data[2] = n;
            } else if (*value_id == ADAPTIVE_VID_STATE) {
                /* live render state, so the app can mirror the board
                 * pixel-for-pixel with the same math:
                 * [0-1] timer lo/hi  [2] blend  [3] speed  [4] val
                 * [5] heldN [6-11] held led idx
                 * [12] hitN then per hit: idx, tick lo, tick hi (newest 5)
                 * [28] kps energy */
                uint16_t t    = (uint16_t)(am_timer - adaptive_wave_epoch);
                value_data[0] = t & 0xFF;
                value_data[1] = t >> 8;
                value_data[2] = adaptive_blend_level;
                value_data[3] = am_get_speed();
                value_data[4] = am_get_val();
                uint8_t hn    = adaptive_held_count > 6 ? 6 : adaptive_held_count;
                value_data[5] = hn;
                for (uint8_t i = 0; i < 6; i++)
                    value_data[6 + i] = (i < hn) ? adaptive_held_leds[i] : 0xFF;
                /* QMK appends new hits at the END of the tracker and old
                 * ones linger for ~65s, so report the NEWEST cn entries.
                 * Reporting [0..cn) sent minute-old hits and dropped the
                 * keys actually being typed. */
                uint8_t total  = g_last_hit_tracker.count;
                uint8_t cn     = total > 5 ? 5 : total;
                uint8_t start  = total - cn;
                value_data[12] = cn;
                for (uint8_t i = 0; i < cn; i++) {
                    value_data[13 + i * 3] = g_last_hit_tracker.index[start + i];
                    value_data[14 + i * 3] = g_last_hit_tracker.tick[start + i] & 0xFF;
                    value_data[15 + i * 3] = g_last_hit_tracker.tick[start + i] >> 8;
                }
                /* 28, not 27: five hits occupy [13..27], so kps at 27
                 * was overwriting the fifth hit's tick high byte. */
                value_data[28] = adaptive_kps_energy;
            }
            break;

        case id_custom_save:
            adaptive_save();
            break;

        default:
            *command_id = id_unhandled;
            break;
    }
}

/* ------------------------------------------------------------------
 * Adaptive backlight
 *
 * Rendering lives in adaptive_effect.inc. This file supplies it with
 * three things via extern:
 *
 *   last_input_time      - timestamp of the last key PRESS
 *   adaptive_keys_held   - is anything physically down right now
 *   adaptive_held_leds[] - which LEDs those held keys sit under
 *
 * The held-key scan reads the matrix directly rather than counting
 * press/release events, so it cannot drift out of sync if a release
 * is ever swallowed by a layer change.
 *
 * matrix_scan_user() is used because Keychron overrides
 * housekeeping_task_kb() WITHOUT calling housekeeping_task_user(), so
 * anything placed in housekeeping_task_user() silently never runs.
 * ------------------------------------------------------------------ */

#ifndef ADAPTIVE_HELD_MAX
#    define ADAPTIVE_HELD_MAX 10
#endif

uint32_t last_input_time              = 0;
uint8_t  adaptive_kps_energy          = 0;
static uint32_t kps_decay_last        = 0;
bool     adaptive_keys_held           = false;
uint8_t  adaptive_held_count          = 0;
uint8_t  adaptive_held_leds[ADAPTIVE_HELD_MAX];
uint32_t adaptive_held_since[ADAPTIVE_HELD_MAX];  /* when each entry went down */

/* synthetic holds injected over USB (simulator mouse clicks). Expire
 * after 8s in case a release packet is lost, so a dropped packet can
 * never leave a key glowing forever. */
#define SYN_HELD_MAX 6
static uint8_t  syn_held_led[SYN_HELD_MAX];
static uint32_t syn_held_at[SYN_HELD_MAX];
static uint8_t  syn_held_n = 0;

static void adaptive_scan_held(void) {
    /* previous frame's list, so a key that stays down keeps its
     * original press timestamp (needed for the ripple/settle modes) */
    uint8_t  prev_leds[ADAPTIVE_HELD_MAX];
    uint32_t prev_since[ADAPTIVE_HELD_MAX];
    uint8_t  prev_n = adaptive_held_count;
    memcpy(prev_leds,  adaptive_held_leds,  sizeof(prev_leds));
    memcpy(prev_since, adaptive_held_since, sizeof(prev_since));

    uint8_t  n   = 0;
    uint32_t now = timer_read32();

    for (uint8_t r = 0; r < MATRIX_ROWS && n < ADAPTIVE_HELD_MAX; r++) {
        matrix_row_t row = matrix_get_row(r);
        if (!row) continue;

        for (uint8_t c = 0; c < MATRIX_COLS && n < ADAPTIVE_HELD_MAX; c++) {
            if (!(row & ((matrix_row_t)1 << c))) continue;
            uint8_t li = g_led_config.matrix_co[r][c];
            if (li == NO_LED) continue;
            uint32_t since = now;
            for (uint8_t j = 0; j < prev_n; j++)
                if (prev_leds[j] == li) { since = prev_since[j]; break; }
            adaptive_held_since[n] = since;
            adaptive_held_leds[n++] = li;
        }
    }

    /* append synthetic holds (dedupe against physically-held keys);
     * their press time is when the inject command arrived */
    for (uint8_t i = 0; i < syn_held_n;) {
        if (timer_elapsed32(syn_held_at[i]) > 8000) { /* lost release */
            syn_held_n--;
            syn_held_led[i] = syn_held_led[syn_held_n];
            syn_held_at[i]  = syn_held_at[syn_held_n];
            continue;
        }
        bool dup = false;
        for (uint8_t j = 0; j < n; j++)
            if (adaptive_held_leds[j] == syn_held_led[i]) { dup = true; break; }
        if (!dup && n < ADAPTIVE_HELD_MAX) {
            adaptive_held_since[n] = syn_held_at[i];
            adaptive_held_leds[n++] = syn_held_led[i];
        }
        i++;
    }

    adaptive_held_count = n;
    adaptive_keys_held  = (n > 0);
}

static void adaptive_inject_hit(uint8_t led, uint8_t pressed) {
    if (led >= AM_LED_COUNT) return;

    if (pressed) {
        /* fire the exact reactive path a real press takes, so the
         * strike ripple/glow on the board is identical. (This branch
         * names it *_handle_key_event, not process_*_matrix.) */
        for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                if (g_led_config.matrix_co[r][c] == led) {
                    am_handle_key_event(r, c, true);
                    r = MATRIX_ROWS;
                    break;
                }
            }
        }
        /* and the same blend/kps side effects process_record_user has */
        last_input_time = timer_read32();
        uint16_t e          = adaptive_kps_energy + 36;
        adaptive_kps_energy = (e > 255) ? 255 : (uint8_t)e;

        for (uint8_t i = 0; i < syn_held_n; i++) {
            if (syn_held_led[i] == led) { syn_held_at[i] = timer_read32(); return; }
        }
        if (syn_held_n < SYN_HELD_MAX) {
            syn_held_led[syn_held_n] = led;
            syn_held_at[syn_held_n]  = timer_read32();
            syn_held_n++;
        }
    } else {
        for (uint8_t i = 0; i < syn_held_n; i++) {
            if (syn_held_led[i] == led) {
                syn_held_n--;
                syn_held_led[i] = syn_held_led[syn_held_n];
                syn_held_at[i]  = syn_held_at[syn_held_n];
                break;
            }
        }
    }
}

void keyboard_post_init_adaptive(void) {
    keyboard_post_init_adaptive_kb();
    adaptive_load();
}

void housekeeping_task_adaptive(void) {
    housekeeping_task_adaptive_kb();

    if (am_get_mode() != AM_MODE_ADAPTIVE) {
        am_mode_noeeprom(AM_MODE_ADAPTIVE);
    }
    adaptive_scan_held();

    /* decay typing-speed energy: full-to-zero in about a second */
    uint32_t now = timer_read32();
    uint32_t dt  = now - kps_decay_last;
    if (dt >= 8) {
        kps_decay_last = now;
        if (dt > 200) dt = 200;
        uint8_t step = (uint8_t)((dt * 255) / 1100);
        if (step == 0) step = 1;
        adaptive_kps_energy = (adaptive_kps_energy > step) ? adaptive_kps_energy - step : 0;
    }
}

bool process_record_adaptive(uint16_t keycode, keyrecord_t *record) {
    if (!process_record_adaptive_kb(keycode, record)) {
        return false;
    }

    /* The effect-step key is repurposed: the module pins the LED mode
     * to ADAPTIVE anyway, so stock BL_STEP / RGB_MOD did nothing. It now
     * cycles presets; the animation restart is the visual confirmation. */
    if (keycode == AM_STEP_KEYCODE_1 || keycode == AM_STEP_KEYCODE_2) {
        if (record->event.pressed) {
            adaptive_switch_slot((uint8_t)((adaptive_ee.active + 1) % adaptive_ee.count));
        }
        return false;
    }

    if (socd_chord_process(keycode, record)) return false; /* recording, or the chord fired */
    if (socd_on) socd_filter(keycode, record);

    if (record->event.pressed) {
        last_input_time = timer_read32();
        uint16_t e      = adaptive_kps_energy + 36; /* ~7 presses to saturate */
        adaptive_kps_energy = (e > 255) ? 255 : (uint8_t)e;
    }

    return true; /* always let QMK handle the pressed key itself */
}