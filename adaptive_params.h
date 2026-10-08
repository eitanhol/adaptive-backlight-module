// Copyright 2025-2026 eitanhol
// SPDX-License-Identifier: GPL-2.0-or-later
/* Shared between keymap.c and led_matrix_user.inc.
 *
 * These used to be #defines that required a recompile to change.
 * They're now runtime values, settable over USB while the board runs.
 *
 * Adding a NEW parameter still requires a reflash - the six USER_
 * slots exist so you can wire something up in the effect math without
 * having to touch the protocol or the app.
 */

#pragma once

#include <stdint.h>

enum adaptive_param_id {
    AP_ATTACK_MS = 0,   /* idle -> active ramp, ms          */
    AP_RELEASE_MS,      /* active -> idle ramp, ms          */
    AP_HOLD_MS,         /* grace before release begins, ms  */
    AP_REACT_FALLOFF,   /* strike glow spread               */
    AP_HELD_FALLOFF,    /* held-key halo spread             */
    AP_WAVE_DIV,        /* idle wave speed divisor          */
    AP_FLICK_DIV,       /* idle flicker speed divisor       */
    AP_FLICK_DEPTH,     /* flicker depth, 0-255             */
    AP_FLICK_SPREAD,    /* per-LED shimmer phase offset     */

    /* formerly spare USER slots, now wired. A value of 0 always means
     * the legacy default, so boards with older saved settings behave
     * identically after a firmware update. */
    AP_WAVE_DIR,      /* was USER_A: 0 horiz, 1 vert, 2 radial, 3 diagonal */
    AP_IDLE_FLOOR,    /* was USER_B: minimum idle brightness, 0-255        */
    AP_STRIKE_MODE,   /* was USER_C: 0 glow, 1 ripple ring, 2 off,
                       * 3 column beam, 4 row+column cross                 */
    AP_STRIKE_DUR,    /* was USER_D: glow lifetime ms; 0 = follow speed    */
    AP_KPS_GAIN,      /* was USER_E: typing-speed board lift; 0 = off      */
    AP_HELD_LEVEL,    /* was USER_F: held glow peak; 0 = full (255)        */

    /* added later; same 0-means-legacy convention */
    AP_IDLE_STYLE,    /* 0 wave, 1 solid, 2 breathe, 3 off, 4 twinkle      */
    AP_BLEND_DEPTH,   /* how much typing hides the idle look; 0 = legacy
                       * full crossfade (same as 255), 1 = pure overlay    */

    AP_STARTUP_STYLE, /* power-on animation: 0 none, 1 sweep, 2 bloom,
                       * 3 rain, 4 sparkle, 5 finale, 6 ignition,
                       * 7 supernova, 8 glitch, 9 comet, 10 surge          */
    AP_LT_STYLE,      /* long-term idle after minutes of no typing:
                       * 0 disabled, else idle style + 1 (1 wave, 2 solid,
                       * 3 breathe, 4 dark, 5 twinkle)                     */
    AP_LT_MINUTES,    /* minutes before long-term idle; 0 = default (5)    */
    AP_LT_LEVEL,      /* long-idle peak brightness; 0 = legacy full (255)  */
    AP_LT_WAVE_DIV,   /* long-idle wave/breathe speed divisor              */
    AP_LT_WAVE_DIR,   /* long-idle wave direction, same codes as WAVE_DIR  */
    AP_LT_FLICK_DEPTH,/* long-idle flicker depth 0-255                     */
    AP_LT_FLICK_DIV,  /* long-idle flicker speed divisor                   */
    AP_LT_FLICK_SPREAD,/* long-idle per-key shimmer phase offset           */

    AP_HELD_MODE,     /* 0 steady halo, 1 ripples while held,
                       * 2 ripples that settle into a halo               */
    AP_HELD_DUR,      /* mode 2: ms of rippling before settling; 0 = 1500 */
    AP_HELD_DELAY,    /* ms a key must be held before the held layer
                       * starts (with a short fade-in), so it can't
                       * stomp the keypress animation; 0 = immediate     */

    AP_BLEND_MODE,    /* 0 = typing dims the idle look everywhere
                       * (legacy); 1 = only where the typing effect
                       * actually lights, per-LED                        */
    AP_WAVE_WIDTH,    /* idle wave band width multiplier, x16 fixed
                       * point (16 = normal); 0 = legacy (16)            */
    AP_PULSE_HIGH,    /* breathing: brightest point 0-255; 0 = off      */
    AP_PULSE_LOW,     /* breathing: dimmest point 0-255                 */
    AP_PULSE_DIV,     /* breathing speed divisor                        */
    AP_LT_WAVE_WIDTH, /* long-idle wave band width, x16; 0 = 16         */
    AP_STARTUP_DUR,   /* startup animation length ms; 0 = 2000          */

    AP_COUNT
};

/* VIA custom channel. Keychron uses via_command_kb() for its own
 * traffic; via_custom_value_command_kb() was unclaimed, so this rides
 * there without disturbing anything they do. */
#define ADAPTIVE_VIA_CHANNEL 0x42

/* extra value ids on the same channel, outside the parameter array */
#define ADAPTIVE_VID_STATE 0x80 /* get: live render state for app mirroring */
#define ADAPTIVE_VID_SPEED 0x90 /* get/set: led matrix global speed          */
#define ADAPTIVE_VID_VAL 0x91   /* get/set: led matrix global brightness     */
#define ADAPTIVE_VID_HUE 0x9B   /* get/set: colour hue 0-255 (RGB boards only;
                                 * white boards read 0 and ignore sets)      */
#define ADAPTIVE_VID_SAT 0x9C   /* get/set: colour saturation 0-255, same      */
#define ADAPTIVE_VID_REBOOT 0x92 /* set: jump to DFU bootloader for flashing   */
#define ADAPTIVE_VID_HIT 0x93    /* set: inject a synthetic keypress [led, down] */
#define ADAPTIVE_VID_RESTART 0x94 /* set: restart the idle animation phase        */
#define ADAPTIVE_VID_STARTUP 0x97 /* set: replay the startup animation now        */
#define ADAPTIVE_VID_LT_PREVIEW 0x98 /* set: engage long idle right now for preview;
                                      * the next real keypress cancels it            */
#define ADAPTIVE_VID_SLOT 0x95    /* set: switch to preset slot [0..count-1]
                                   * get: [active, dirty, long-idle level,
                                   *       preset count, preset max]             */
#define ADAPTIVE_VID_SLOT_ADD 0x99 /* set: add a preset (copy of the live values)
                                    * and make it active; no-op at the max       */
#define ADAPTIVE_VID_SLOT_DEL 0x9A /* set: delete preset [idx]; later presets
                                    * shift down; the last one cannot be deleted */
#define ADAPTIVE_VID_REVERT 0x96  /* set: reload the active preset, discarding
                                   * unsaved live changes                         */

/* Board self-description, so the app needs no per-keyboard tables.
 * All three are get-only. value_data has 29 usable bytes. */
#define ADAPTIVE_GEOM_VERSION 1
#define ADAPTIVE_VID_GEOM_INFO 0xA0   /* get: [geom ver, led count, matrix rows,
                                       * matrix cols, flags, AP_COUNT, presets]
                                       * flags bit0 = RGB matrix (else mono)   */
#define ADAPTIVE_VID_GEOM_POINTS 0xA1 /* get: in [first led] ->
                                       * out [first, n, x0,y0, x1,y1 ...]
                                       * up to 13 LEDs per packet, QMK 0-224 x
                                       * 0-64 coordinate space                 */
#define ADAPTIVE_VID_GEOM_MATRIX 0xA2 /* get: in [row, first col] ->
                                       * out [row, first col, n, led idx per col]
                                       * 0xFF = no LED under that key          */
#define ADAPTIVE_GEOM_POINTS_PER_PKT 13
#define ADAPTIVE_GEOM_COLS_PER_PKT 26

/* Preset slots live on the board itself so the physical BL key can
 * cycle them with no app running. */
#define ADAPTIVE_PRESETS 4

/* held-key list size, shared by keymap.c and the effect */
#define ADAPTIVE_HELD_MAX 10

extern uint16_t adaptive_params[AP_COUNT];