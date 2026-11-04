# v0.1.0

First public release. The whole steno engine runs inside an ESP32, and the keyboard shows up on a PC as a normal Bluetooth keyboard. No Plover and no driver on the computer.

## What's in it

- Plover-compatible engine in C: dictionary lookup, translation, formatting, undo
- Plover's `main.json` (147,424 entries) packed into a 2.2 MB `dict.bin` that is read straight from flash
- Orthography, fingerspelling, numbers, capitalisation and punctuation
- Retro commands: toggle asterisk, delete space, insert space, repeat last stroke
- Keyboard shortcuts as strokes (copy, paste, cut, select all, undo, arrows, Alt+Tab). More can go in `tools/user.json`
- Suspend / resume steno with `PHRO*F` / `PHRO*PB`
- 24 touch pads on two MPR121 chips, each on its own I2C bus
- BLE HID keyboard with bonding, so it reconnects after the first pairing
- PC test tools: `demo.py` to try strokes without hardware, a smoke test, and a differential test against real Plover
- GitHub Actions runs the smoke test (with ASan and UBSan) on every push

## Good to know

- **Only tested on Windows.** It should work with other hosts that accept a BLE keyboard, but I haven't tried.
- **Set the PC's input language to English (US).** The keyboard sends US key positions.
- **USB power only**, no battery yet.
- **Built with ESP-IDF 6.1** on the original ESP32 with 4 MB flash. Other versions are untested.
- Not supported yet: prefix strokes, `{=conditional}`, `{MODE:...}`, user macros, non-ASCII characters.
- The dictionary can only be changed by re-flashing (`python tools/convert_dict.py`, then `idf.py flash`).

## Licensing

GPL-2.0-or-later (see `LICENSE`). The dictionary, wordlist, orthography rules and translation logic come from, or are ported from, [Plover](https://github.com/openstenoproject/plover), which uses the same license.
