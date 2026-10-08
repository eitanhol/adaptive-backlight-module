// Copyright 2025-2026 eitanhol
// SPDX-License-Identifier: GPL-2.0-or-later
/* RGB boards: the effect value scales the board's own colour, so hue and
 * saturation stay whatever the user set (Fn keys, VIA, or the app) and
 * the module only shapes brightness - a preset looks the same on a white
 * board and on an RGB board set to white. */
RGB_MATRIX_EFFECT(ADAPTIVE)

#ifdef RGB_MATRIX_CUSTOM_EFFECT_IMPLS
#    define AM_USE_LIMITS RGB_MATRIX_USE_LIMITS
#    define AM_TEST_LED_FLAGS RGB_MATRIX_TEST_LED_FLAGS
#    define AM_SPEED rgb_matrix_config.speed
#    define am_check_finished_leds rgb_matrix_check_finished_leds
/* parameter names must not collide with struct members (.v) */
#    define AM_OUTPUT(_idx, _val)                                 \
        do {                                                     \
            hsv_t _h = rgb_matrix_config.hsv;                    \
            _h.v     = scale8((_val), _h.v);                     \
            rgb_t _c = hsv_to_rgb(_h);                           \
            rgb_matrix_set_color((_idx), _c.r, _c.g, _c.b);      \
        } while (0)
#    include "adaptive_effect.inc"
#endif
