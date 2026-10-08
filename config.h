// Copyright 2025-2026 eitanhol
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* Storage for adaptive presets: magic(2) + active slot(2) +
 * 4 slots x 40 padded uint16 params (320) + count(2) + spare(2) = 328.
 * Per-slot padding to 40 means future params won't force another
 * resize (a resize moves the datablock and wipes saved settings once).
 * Without this, eeconfig_read_user_datablock() doesn't exist and
 * saved settings won't survive a power cycle. */
#define EECONFIG_USER_DATA_SIZE 328

/* the strike layer reads g_last_hit_tracker, which only exists when the
 * lighting system tracks keypresses. Keychron boards define these already;
 * identical redefinitions are harmless. */
#define LED_MATRIX_KEYPRESSES
#define RGB_MATRIX_KEYPRESSES
