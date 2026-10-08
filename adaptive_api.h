// Copyright 2025-2026 eitanhol
// SPDX-License-Identifier: GPL-2.0-or-later
/* One name for each lighting call the module makes, mapped onto QMK's
 * single-colour LED matrix or its RGB matrix, whichever the board has.
 * Everything else in the module is written against these am_* names so
 * the effect, the protocol and the key handling are identical on both.
 *
 * On RGB boards the effect still only shapes brightness; the colour is
 * the board's own hue/saturation (Fn keys, VIA, or our 0x9B/0x9C), so a
 * preset looks the same on a white board and an RGB board set to white.
 */
#pragma once

#if defined(RGB_MATRIX_ENABLE)

#    include "rgb_matrix.h"
#    define AM_RGB 1
#    define AM_LED_COUNT RGB_MATRIX_LED_COUNT
#    define am_timer g_rgb_timer
#    define AM_MODE_ADAPTIVE RGB_MATRIX_CUSTOM_ADAPTIVE
#    define am_get_mode() rgb_matrix_get_mode()
#    define am_mode_noeeprom(m) rgb_matrix_mode_noeeprom(m)
#    define am_get_speed() rgb_matrix_get_speed()
#    define am_set_speed_noeeprom(s) rgb_matrix_set_speed_noeeprom(s)
#    define am_get_val() rgb_matrix_get_val()
#    define am_set_val_noeeprom(v) rgb_matrix_sethsv_noeeprom(rgb_matrix_config.hsv.h, rgb_matrix_config.hsv.s, (v))
#    define am_get_hue() (rgb_matrix_config.hsv.h)
#    define am_get_sat() (rgb_matrix_config.hsv.s)
#    define am_set_hue_noeeprom(h) rgb_matrix_sethsv_noeeprom((h), rgb_matrix_config.hsv.s, rgb_matrix_config.hsv.v)
#    define am_set_sat_noeeprom(s) rgb_matrix_sethsv_noeeprom(rgb_matrix_config.hsv.h, (s), rgb_matrix_config.hsv.v)
#    define am_handle_key_event(r, c, p) rgb_matrix_handle_key_event((r), (c), (p))
/* Keychron's RGB keymaps put the effect-step key on UG_NEXT; RM_NEXT is
 * the current QMK name for the same job */
#    define AM_STEP_KEYCODE_1 QK_UNDERGLOW_MODE_NEXT
#    define AM_STEP_KEYCODE_2 QK_RGB_MATRIX_MODE_NEXT

#elif defined(LED_MATRIX_ENABLE)

#    include "led_matrix.h"
#    define AM_RGB 0
#    define AM_LED_COUNT LED_MATRIX_LED_COUNT
#    define am_timer g_led_timer
#    define AM_MODE_ADAPTIVE LED_MATRIX_CUSTOM_ADAPTIVE
#    define am_get_mode() led_matrix_get_mode()
#    define am_mode_noeeprom(m) led_matrix_mode_noeeprom(m)
#    define am_get_speed() led_matrix_get_speed()
#    define am_set_speed_noeeprom(s) led_matrix_set_speed_noeeprom(s)
#    define am_get_val() led_matrix_get_val()
#    define am_set_val_noeeprom(v) led_matrix_set_val_noeeprom(v)
#    define am_get_hue() 0
#    define am_get_sat() 0
#    define am_set_hue_noeeprom(h) ((void)(h))
#    define am_set_sat_noeeprom(s) ((void)(s))
#    define am_handle_key_event(r, c, p) led_matrix_handle_key_event((r), (c), (p))
#    define AM_STEP_KEYCODE_1 QK_BACKLIGHT_STEP
#    define AM_STEP_KEYCODE_2 QK_BACKLIGHT_STEP

#else
#    error "adaptive: this board has neither LED_MATRIX_ENABLE nor RGB_MATRIX_ENABLE - the module needs per-key lighting"
#endif
