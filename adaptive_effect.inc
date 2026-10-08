// Copyright 2025-2026 eitanhol
// SPDX-License-Identifier: GPL-2.0-or-later
/* Custom LED matrix effect: ADAPTIVE
 *
 * Renders three layers and mixes them:
 *
 *   idle   - slow flickering travelling wave
 *   strike - reactive glow that decays after each keypress
 *   held   - steady glow under keys currently being held down
 *
 * strike and held together form the "active" look; a single blend
 * level crossfades between that and idle.
 *
 * Shared by led_matrix_user.inc and rgb_matrix_user.inc, which define
 * the AM_* output macros for their lighting system before including
 * this file. Selected with am_mode_noeeprom(AM_MODE_ADAPTIVE).
 */


/* ---- tuning is now runtime ------------------------------------
 * Values live in adaptive_params[] and are settable over USB while
 * the board runs. Defaults are in keymap.c. Index names come from
 * adaptive_params.h.
 * -------------------------------------------------------------- */

#define AP(x) adaptive_params[x]

#include "adaptive_params.h"
#include "adaptive_api.h"

/* supplied by keymap.c */
extern uint32_t last_input_time;
extern bool     adaptive_keys_held;
extern uint8_t  adaptive_held_count;
extern uint8_t  adaptive_held_leds[];
extern uint32_t adaptive_held_since[];
extern uint8_t  adaptive_kps_energy;
extern uint32_t adaptive_wave_epoch;
extern uint32_t adaptive_su_epoch;
extern uint32_t last_input_time;
extern uint8_t  adaptive_lt_level;

/* Idle value for a given style; used for both the short-term idle and
 * the long-term (multi-minute) idle so the two can crossfade. */
/* Idle value for a style with an explicit parameter set, so the
 * short-term and long-term idle layers are fully independent. */
static uint8_t adaptive_idle_for(uint8_t style, uint8_t dir, uint8_t depth,
                                 uint8_t spread, uint8_t wmul,
                                 uint8_t i, uint8_t px, uint8_t py,
                                 uint8_t wave_time, uint8_t flicker_time,
                                 uint8_t flicker_time2) {
    uint8_t shimmer = sin8((uint8_t)(i * spread) + flicker_time);
    switch (style) {
        case 1: return 255;                                      /* solid   */
        case 2: return sin8(wave_time);                          /* breathe */
        case 3: return 0;                                        /* off     */
        case 4: {                                                /* twinkle */
            /* product of two shimmer components at different rates:
             * keys only peak when both align, so they sparkle
             * independently instead of breathing in phase order */
            uint8_t s2 = sin8((uint8_t)(i * 151) + flicker_time2);
            uint8_t tw = scale8(shimmer, s2);
            return (uint8_t)((255 - depth) + scale8(tw, depth));
        }
        default: {                                               /* wave    */
            uint8_t coord;
            switch (dir) {
                case 1:  coord = (uint8_t)(py * 3); break;       /* vertical   */
                case 2: {                                        /* radial     */
                    int16_t rx = px - 112, ry = py - 32;
                    coord = (uint8_t)(sqrt16(rx * rx + ry * ry) * 2);
                    break;
                }
                case 3:  coord = (uint8_t)(px + py * 2); break;  /* diagonal   */
                default: coord = px; break;                      /* horizontal */
            }
            /* wmul is a x16 fixed-point band-width scaler: 16 = one
             * wave across the board, 32 = two, 8 = one double-wide */
            uint8_t cw   = (uint8_t)(((uint16_t)coord * wmul) >> 4);
            uint8_t wave = sin8(cw - wave_time);
            return scale8(wave, (uint8_t)((255 - depth) + scale8(shimmer, depth)));
        }
    }
}

/* 0 = fully idle, 255 = fully active. Slews rather than jumps.
 * Non-static: keymap.c reports it in the app-mirroring state packet. */
uint8_t         adaptive_blend_level = 0;
static uint32_t blend_last           = 0;

static uint8_t adaptive_blend(void) {
    uint32_t dt = am_timer - blend_last;
    blend_last  = am_timer;
    if (dt == 0) return adaptive_blend_level; /* same frame, nothing to advance */
    if (dt > 200) dt = 200;          /* clamp after sleep/USB stalls   */

    /* Active while a key is physically down, OR briefly after the last
     * press. The held check reads the matrix, so holding a key keeps
     * the active look instead of decaying into idle. */
    bool active = adaptive_keys_held || (timer_elapsed32(last_input_time) < AP(AP_HOLD_MS));

    if (active) {
        uint32_t step = (255UL * dt) / AP(AP_ATTACK_MS);
        adaptive_blend_level   = (adaptive_blend_level + step >= 255) ? 255 : (uint8_t)(adaptive_blend_level + step);
    } else {
        uint32_t step = (255UL * dt) / AP(AP_RELEASE_MS);
        adaptive_blend_level   = (adaptive_blend_level <= step) ? 0 : (uint8_t)(adaptive_blend_level - step);
    }

    return adaptive_blend_level;
}

/* Frame-consistent snapshot of everything the effect reads that can
 * change mid-frame. The board renders LEDs in chunks across several
 * task calls; without this, held/blend state mutating between chunks
 * tears the frame - later chunks (the RIGHT side of the board) render
 * different state than earlier ones, which showed up as right-side
 * flicker while typing on the left. */
static uint8_t  fr_blend, fr_lt, fr_held_n, fr_kps;
static uint8_t  fr_held_led[ADAPTIVE_HELD_MAX];
static uint32_t fr_held_t[ADAPTIVE_HELD_MAX];   /* hold duration, ms */

bool ADAPTIVE(effect_params_t* params) {
    AM_USE_LIMITS(led_min, led_max);

    uint8_t speed = AM_SPEED;

    if (led_min == 0) { /* first chunk of the frame: snapshot */
        fr_blend  = adaptive_blend();
        fr_kps    = adaptive_kps_energy;
        fr_held_n = adaptive_held_count;
        for (uint8_t j = 0; j < fr_held_n; j++) {
            fr_held_led[j]  = adaptive_held_leds[j];
            uint32_t since  = adaptive_held_since[j];
            /* a key pressed after this frame's timer snapshot would
             * underflow to ~4 billion ms and blast a full halo for one
             * frame - the right-side flicker bug */
            fr_held_t[j] = (am_timer >= since) ? (am_timer - since) : 0;
        }
        uint8_t ltb = 0;
        if (AP(AP_LT_STYLE)) {
            uint32_t mins    = AP(AP_LT_MINUTES) ? AP(AP_LT_MINUTES) : 5;
            uint32_t thr     = mins * 60000u;
            uint32_t idle_el = am_timer - last_input_time;
            if (idle_el > thr) {
                uint32_t over = idle_el - thr;
                ltb = over > 4080 ? 255 : (uint8_t)(over / 16);
            }
        }
        fr_lt             = ltb;
        adaptive_lt_level = ltb;
    }
    uint8_t blend    = fr_blend;
    uint8_t lt_blend = fr_lt;

    /* phase runs from the restart epoch, not boot, so the app can
     * snap board + sim to phase zero at the same instant */
    uint16_t anim_t      = (uint16_t)(am_timer - adaptive_wave_epoch);
    uint8_t wave_time     = scale16by8(anim_t, (speed / AP(AP_WAVE_DIV)) + 1);
    uint8_t flicker_time  = scale16by8(anim_t, (speed / AP(AP_FLICK_DIV)) + 1);
    uint8_t flicker_time2 = scale16by8(anim_t, (speed / AP(AP_FLICK_DIV)) + 9);
    uint8_t pulse_time    = scale16by8(anim_t, (speed / AP(AP_PULSE_DIV)) + 1);
    /* the long-idle layer has an entirely independent parameter set */
    uint8_t wave_time_lt     = scale16by8(anim_t, (speed / AP(AP_LT_WAVE_DIV)) + 1);
    uint8_t flicker_time_lt  = scale16by8(anim_t, (speed / AP(AP_LT_FLICK_DIV)) + 1);
    uint8_t flicker_time2_lt = scale16by8(anim_t, (speed / AP(AP_LT_FLICK_DIV)) + 9);

    /* startup overlay window: epoch is 0 at boot (plays once at
     * power-on) and can be reset over USB to replay for previewing */
    uint32_t su_t = am_timer - adaptive_su_epoch;

    uint8_t hit_count = g_last_hit_tracker.count;

    for (uint8_t i = led_min; i < led_max; i++) {
        AM_TEST_LED_FLAGS();

        uint8_t px = g_led_config.point[i].x;
        uint8_t py = g_led_config.point[i].y;

        /* ---- idle layer: short-term style, crossfaded into the
         * long-term style once lt_blend rises ---- */
        uint8_t wmul     = AP(AP_WAVE_WIDTH) ? (uint8_t)AP(AP_WAVE_WIDTH) : 16;
        uint8_t idle_val = adaptive_idle_for((uint8_t)AP(AP_IDLE_STYLE),
                                             (uint8_t)AP(AP_WAVE_DIR),
                                             (uint8_t)AP(AP_FLICK_DEPTH),
                                             (uint8_t)AP(AP_FLICK_SPREAD), wmul,
                                             i, px, py, wave_time, flicker_time, flicker_time2);
        if (lt_blend) {
            uint8_t ltw    = AP(AP_LT_WAVE_WIDTH) ? (uint8_t)AP(AP_LT_WAVE_WIDTH) : 16;
            uint8_t lt_val = adaptive_idle_for((uint8_t)(AP(AP_LT_STYLE) - 1),
                                               (uint8_t)AP(AP_LT_WAVE_DIR),
                                               (uint8_t)AP(AP_LT_FLICK_DEPTH),
                                               (uint8_t)AP(AP_LT_FLICK_SPREAD), ltw,
                                               i, px, py, wave_time_lt,
                                               flicker_time_lt, flicker_time2_lt);
            uint8_t lt_lvl = AP(AP_LT_LEVEL) ? (uint8_t)AP(AP_LT_LEVEL) : 255;
            lt_val         = scale8(lt_val, lt_lvl);
            idle_val = qadd8(scale8(idle_val, 255 - lt_blend), scale8(lt_val, lt_blend));
        }
        /* breathing: oscillates the whole idle look between the low
         * and high points at its own speed, whatever the style */
        if (AP(AP_PULSE_HIGH)) {
            uint8_t hi = (uint8_t)AP(AP_PULSE_HIGH);
            uint8_t lo = (uint8_t)AP(AP_PULSE_LOW);
            if (lo > hi) { uint8_t t = lo; lo = hi; hi = t; }
            uint8_t mod = (uint8_t)(lo + scale8(sin8(pulse_time), (uint8_t)(hi - lo)));
            idle_val    = scale8(idle_val, mod);
        }
        uint8_t floor_v = (uint8_t)AP(AP_IDLE_FLOOR);
        if (idle_val < floor_v) idle_val = floor_v;

        /* ---- strike layer: decays after each press ---- */
        uint8_t active_val = 0;
        for (uint8_t j = 0; j < hit_count && AP(AP_STRIKE_MODE) != 2; j++) {
            int16_t  dx   = px - g_last_hit_tracker.x[j];
            int16_t  dy   = py - g_last_hit_tracker.y[j];
            uint8_t  adx  = (uint8_t)(dx < 0 ? -dx : dx);
            uint8_t  ady  = (uint8_t)(dy < 0 ? -dy : dy);
            uint8_t  dist = sqrt16(dx * dx + dy * dy);
            uint16_t dur  = AP(AP_STRIKE_DUR);
            uint16_t tick = dur ? (uint16_t)(((uint32_t)g_last_hit_tracker.tick[j] * 255) / dur)
                                : scale16by8(g_last_hit_tracker.tick[j], speed);

            if (AP(AP_STRIKE_MODE) == 1) {
                /* expanding ring: brightest where distance matches the
                 * ring radius, which grows with tick */
                int16_t d = (int16_t)(dist * AP(AP_REACT_FALLOFF)) - (int16_t)tick;
                if (d < 0) d = -d;
                uint16_t edge = (uint16_t)d * 3;
                if (edge > 255 || tick > 255) edge = 255;
                active_val = qadd8(active_val, 255 - (uint8_t)edge);
            } else {
                /* glow-family: distance metric picks the shape.
                 * 0 glow: round. 3 beam: |dx| only, lights the column.
                 * 4 cross: min(|dx|,|dy|), lights the row and column. */
                uint8_t  d2     = dist;
                if (AP(AP_STRIKE_MODE) == 3) d2 = adx;
                if (AP(AP_STRIKE_MODE) == 4) d2 = (adx < ady) ? adx : ady;
                uint16_t effect = tick + d2 * AP(AP_REACT_FALLOFF);
                if (effect > 255) effect = 255;
                active_val = qadd8(active_val, 255 - effect);
            }
        }

        /* ---- held layer.
         * Mode 0: steady halo (no decay - without it, holding a key
         * would sit at full blend over a decayed strike = black board).
         * Mode 1: ripples radiate from the key as long as it's held.
         * Mode 2: ripples for HELD_DUR ms, then settle into the halo.
         * HELD_DELAY postpones the whole layer (with a 255ms fade-in)
         * so a quick press shows only the keypress animation. ---- */
        for (uint8_t j = 0; j < fr_held_n; j++) {
            uint8_t  li   = fr_held_led[j];
            int16_t  dx   = px - g_led_config.point[li].x;
            int16_t  dy   = py - g_led_config.point[li].y;
            uint8_t  dist = sqrt16(dx * dx + dy * dy);

            uint32_t hold_t = fr_held_t[j];
            uint16_t dly    = AP(AP_HELD_DELAY);
            if (hold_t < dly) continue;
            hold_t -= dly;
            uint8_t fin = hold_t > 255 ? 255 : (uint8_t)hold_t; /* fade-in */

            uint16_t effect = dist * AP(AP_HELD_FALLOFF);
            if (effect > 255) effect = 255;
            uint8_t halo = 255 - (uint8_t)effect;

            uint8_t hv;
            uint8_t mode = (uint8_t)AP(AP_HELD_MODE);
            if (mode == 0) {
                hv = halo;
            } else {
                /* ripple train: the same narrow expanding ring as the
                 * typing Ripple style, emitted continuously - two
                 * staggered rings so a new one launches while the last
                 * is still travelling */
                uint8_t  phase = (uint8_t)scale16by8((uint16_t)hold_t, (speed / 8) + 1);
                uint16_t dh    = (uint16_t)(dist * AP(AP_HELD_FALLOFF));
                uint8_t  rip   = 0;
                int16_t  d1    = (int16_t)dh - (int16_t)phase;
                if (d1 < 0) d1 = -d1;
                uint16_t e1 = (uint16_t)d1 * 3;
                if (e1 <= 255) rip = (uint8_t)(255 - e1);
                uint8_t  p2 = (uint8_t)(phase + 128);
                int16_t  d2 = (int16_t)dh - (int16_t)p2;
                if (d2 < 0) d2 = -d2;
                uint16_t e2 = (uint16_t)d2 * 3;
                if (e2 <= 255) {
                    uint8_t r2 = (uint8_t)(255 - e2);
                    if (r2 > rip) rip = r2;
                }
                if (mode == 1) {
                    hv = rip;
                } else {
                    uint32_t dur = AP(AP_HELD_DUR) ? AP(AP_HELD_DUR) : 1500;
                    if (hold_t < dur) {
                        hv = rip;
                    } else {
                        uint32_t ov = hold_t - dur;
                        uint8_t  mx = ov > 255 ? 255 : (uint8_t)ov; /* crossfade */
                        hv = qadd8(scale8(rip, 255 - mx), scale8(halo, mx));
                    }
                }
            }
            uint8_t level = AP(AP_HELD_LEVEL) ? (uint8_t)AP(AP_HELD_LEVEL) : 255;
            active_val    = qadd8(active_val, scale8(scale8(hv, fin), level));
        }

        /* ---- typing-speed lift: fast typing raises the whole board ---- */
        if (AP(AP_KPS_GAIN)) {
            uint8_t lift = scale8(fr_kps, (uint8_t)AP(AP_KPS_GAIN));
            if (active_val < lift) active_val = lift;
        }

        /* ---- crossfade. AP_BLEND_DEPTH sets how much the typing look
         * suppresses the idle look: 0 or 255 = fully (legacy behavior),
         * 1 = not at all (strikes overlay the idle animation). ---- */
        uint8_t bd = AP(AP_BLEND_DEPTH) ? (uint8_t)AP(AP_BLEND_DEPTH) : 255;
        uint8_t mixed;
        if (AP(AP_BLEND_MODE) == 1) {
            /* local: the idle look dims only where the typing effect
             * actually lights, proportional to its brightness there -
             * a ripple ring punches through the idle wave only along
             * the ring itself */
            uint8_t ea = scale8(active_val, blend);
            mixed = qadd8(scale8(idle_val, 255 - scale8(ea, bd)), ea);
        } else {
            mixed = qadd8(scale8(idle_val, 255 - scale8(blend, bd)), scale8(active_val, blend));
        }

        /* ---- startup animation: one-shot overlay after power-on ---- */
        uint32_t su_T = AP(AP_STARTUP_DUR) ? AP(AP_STARTUP_DUR) : 2000;
        if (AP(AP_STARTUP_STYLE) && su_t < su_T) {
            uint32_t su_body = su_T - su_T / 5;   /* last fifth fades out */
            uint8_t  su_val  = 0;
            uint8_t  st      = (uint8_t)AP(AP_STARTUP_STYLE);
            if (st == 1) {                                       /* sweep */
                uint16_t pos = (uint16_t)((su_t * 224) / su_body);
                int16_t  d   = (int16_t)px - (int16_t)pos;
                if (d < 0) d = -d;
                uint16_t v = (uint16_t)d * 4;
                if (v <= 255) su_val = (uint8_t)(255 - v);
            } else if (st == 2) {                                /* bloom */
                int16_t  rx   = px - 112, ry = py - 32;
                uint8_t  dist = sqrt16(rx * rx + ry * ry);
                uint16_t pos  = (uint16_t)((su_t * 130) / su_body);
                int16_t  d    = (int16_t)dist - (int16_t)pos;
                if (d < 0) d = -d;
                uint16_t v = (uint16_t)d * 6;
                if (v <= 255) su_val = (uint8_t)(255 - v);
            } else if (st == 3) {                                /* rain */
                /* per-column staggered drops falling top to bottom */
                uint16_t prog = (uint16_t)((su_t * 320) / su_T);
                uint8_t  stag = sin8((uint8_t)(px * 13)) >> 2;
                int16_t  f    = (int16_t)prog - (int16_t)stag;
                int16_t  d    = f - (int16_t)(py * 4);
                if (d >= 0) {
                    uint16_t v = (uint16_t)d * 6;
                    if (v <= 255) su_val = (uint8_t)(255 - v);
                }
            } else if (st == 4) {                                /* sparkle */
                uint8_t s1 = sin8((uint8_t)(i * 151) + (uint8_t)(su_t >> 1));
                uint8_t s2 = sin8((uint8_t)(i * 89) + (uint8_t)(su_t >> 2) + 60);
                uint8_t tw = scale8(s1, s2);
                if (tw > 170) su_val = (uint8_t)((tw - 170) * 3);
            } else if (st == 5) {                            /* finale */
                uint32_t half = su_T / 2;
                if (su_t < half) {
                    /* two fronts race from the edges to the middle */
                    uint16_t pos = (uint16_t)((su_t * 112) / half);
                    int16_t  a   = (int16_t)px - (int16_t)pos;
                    if (a < 0) a = -a;
                    int16_t  b   = (int16_t)(224 - px) - (int16_t)pos;
                    if (b < 0) b = -b;
                    int16_t  m   = a < b ? a : b;
                    uint16_t v   = (uint16_t)m * 4;
                    if (v <= 255) su_val = (uint8_t)(255 - v);
                } else {
                    /* ...then the whole board flashes and decays */
                    su_val = (uint8_t)(255 - ((su_t - half) * 255) / (su_T - half));
                }
            } else if (st == 6) {                            /* ignition */
                /* F1 start: row bands punch on bottom-to-top, hold,
                 * cut to black, then one fast wave clears the board */
                uint32_t pa = su_T / 2;
                uint32_t pb = pa + su_T / 16;
                if (su_t < pa) {
                    uint8_t  band = (uint8_t)((64 - py) / 13);
                    uint32_t ton  = (band * pa) / 5;
                    if (su_t >= ton) {
                        uint32_t age = su_t - ton;
                        su_val = age >= 120 ? 255 : (uint8_t)((age * 255) / 120);
                    }
                } else if (su_t >= pb) {
                    uint32_t sb2 = su_body - pb;
                    uint16_t pos = (uint16_t)(((su_t - pb) * 240) / sb2);
                    int16_t  d   = (int16_t)px - (int16_t)pos;
                    if (d < 0) d = -d;
                    uint16_t v = (uint16_t)d * 8;
                    if (v <= 255) su_val = (uint8_t)(255 - v);
                }
            } else if (st == 7) {                            /* supernova */
                /* ring collapses to a point, beat of darkness, then
                 * explodes back out with a sparkling tail */
                int16_t  rx   = px - 112, ry = py - 32;
                uint8_t  dist = sqrt16(rx * rx + ry * ry);
                uint32_t pa   = (su_T * 2) / 5;
                uint32_t pb   = pa + su_T / 8;
                if (su_t < pa) {
                    uint16_t pos = (uint16_t)(130 - (su_t * 130) / pa);
                    int16_t  d   = (int16_t)dist - (int16_t)pos;
                    if (d < 0) d = -d;
                    uint16_t v = (uint16_t)d * 5;
                    if (v <= 255) su_val = (uint8_t)(255 - v);
                } else if (su_t < pb) {
                    if (dist <= 12) su_val = 255;            /* held pinprick */
                } else {
                    uint16_t pos = (uint16_t)(((su_t - pb) * 200) / (su_T - pb));
                    if (dist <= pos) {
                        uint16_t v  = (uint16_t)(pos - dist) * 3;
                        uint8_t  s1 = sin8((uint8_t)(i * 151 + (su_t >> 1)));
                        uint8_t  s2 = sin8((uint8_t)(i * 89 + (su_t >> 2) + 60));
                        uint8_t  tw = scale8(s1, s2);
                        if (v <= 255) su_val = (uint8_t)(255 - v);
                        if (tw > 200) su_val = qadd8(su_val, (uint8_t)((tw - 200) * 4));
                    } else {
                        uint16_t v = (uint16_t)(dist - pos) * 8;
                        if (v <= 255) su_val = (uint8_t)(255 - v);
                    }
                }
            } else if (st == 8) {                            /* glitch */
                /* random blocks strobe faster and denser, then the
                 * whole board snaps on and decays into idle */
                uint32_t q = (su_t * 256) / su_T;
                if (q >= 192) {
                    su_val = 255;
                } else {
                    uint32_t n    = (q * q) >> 9;
                    uint8_t  cell = (uint8_t)((py >> 4) * 8 + (px >> 5));
                    uint8_t  h    = sin8((uint8_t)(cell * 47 + n * 101));
                    uint8_t  thr  = (uint8_t)(200 - (q >> 1));
                    if (h > thr) su_val = 255;
                }
            } else if (st == 9) {                            /* comet */
                /* a bright head snakes through every row boustrophedon,
                 * accelerating, with a fading one-row tail */
                uint8_t  row = (uint8_t)(py / 11);
                uint16_t lin = (uint16_t)(row * 224 + ((row & 1) ? (224 - px) : px));
                uint32_t q   = (su_t * 256) / su_body;
                uint32_t D   = (q * q * 1360) >> 16;
                if (D >= lin) {
                    uint32_t d = D - lin;
                    if (d <= 255) su_val = (uint8_t)(255 - d);
                }
            } else {                                         /* surge */
                /* center-weighted heartbeat pulses rising in tempo and
                 * strength until the board overloads into a crackle */
                uint32_t pa = (su_T * 7) / 10;
                if (su_t < pa) {
                    uint32_t q     = (su_t * 256) / pa;
                    uint16_t phase = (uint16_t)((q * q) >> 6);
                    uint8_t  beat  = sin8((uint8_t)phase);
                    uint8_t  b2    = beat > 128 ? (uint8_t)((beat - 128) * 2) : 0;
                    uint8_t  amp   = (uint8_t)(64 + ((q * 3) >> 2));
                    int16_t  rx    = px - 112, ry = py - 32;
                    uint8_t  dist  = sqrt16(rx * rx + ry * ry);
                    su_val = scale8(scale8(b2, amp), (uint8_t)(255 - dist));
                } else {
                    uint8_t s1 = sin8((uint8_t)(i * 151 + su_t));
                    uint8_t s2 = sin8((uint8_t)(i * 89 + (su_t >> 1) + 60));
                    uint8_t tw = scale8(s1, s2);
                    su_val = qsub8(255, (uint8_t)(tw >> 1));
                }
            }
            uint8_t su_mix = su_t < su_body ? 255
                           : (uint8_t)(255 - ((su_t - su_body) * 255) / (su_T - su_body));
            mixed = qadd8(scale8(mixed, 255 - su_mix), scale8(su_val, su_mix));
        }

        AM_OUTPUT(i, mixed);
    }

    return am_check_finished_leds(led_max);
}

