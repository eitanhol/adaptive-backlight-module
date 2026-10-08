// Copyright 2025-2026 eitanhol
// SPDX-License-Identifier: GPL-2.0-or-later
/* single-colour boards: the effect value drives LED brightness directly */
LED_MATRIX_EFFECT(ADAPTIVE)

#ifdef LED_MATRIX_CUSTOM_EFFECT_IMPLS
#    define AM_USE_LIMITS LED_MATRIX_USE_LIMITS
#    define AM_TEST_LED_FLAGS LED_MATRIX_TEST_LED_FLAGS
#    define AM_SPEED led_matrix_eeconfig.speed
#    define am_check_finished_leds led_matrix_check_finished_leds
#    define AM_OUTPUT(i, v) led_matrix_set_value((i), scale8((v), led_matrix_eeconfig.val))
#    include "adaptive_effect.inc"
#endif
