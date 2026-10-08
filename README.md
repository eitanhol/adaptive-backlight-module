# Adaptive backlight - QMK community module

A per-key lighting effect for QMK keyboards with `led_matrix` (single-colour)
or `rgb_matrix` backlighting. It reacts to typing, idles with a configurable
animation, keeps up to four presets in EEPROM, and exposes every parameter over
VIA's raw HID protocol (custom channel `0x42`) so a host app can tune it live.
Also includes a SOCD (last-input-priority) filter for A/D.

Built on the Keychron K10 V2 and compiled for other Keychron boards from
Keychron's QMK fork (branch `wls_2025q1`).

## Attach to a board

Copy this directory to `modules/adaptive/` in your QMK tree, then give the
board a keymap that is a copy of its stock `via` keymap plus this file:

```json
{ "modules": ["adaptive"] }
```

saved as `keymap.json` in that keymap directory. Compile as usual:

```
qmk compile -kb keychron/k10_version_2/ansi/white -km adaptive
```

No keymap or board source is edited; the module hooks `process_record`,
`housekeeping_task`, `keyboard_post_init` and `via_custom_value_command_kb`.

## Files

- `adaptive.c` - hooks, VIA channel `0x42` protocol, presets, SOCD
- `adaptive_effect.inc` - the effect body, shared by both lighting drivers
- `led_matrix_user.inc` / `rgb_matrix_user.inc` - thin wrappers per driver
- `adaptive_api.h` - the mono/RGB shim
- `adaptive_params.h` - parameter ids and value ids (the protocol)
- `config.h` - the EEPROM user block (328 bytes)

## License

GPL-2.0-or-later, like QMK. See `LICENSE`.
