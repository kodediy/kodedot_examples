# DOOM on the Kode Dot

DOOM (1993) running natively on the Kode Dot v2: the **doomgeneric** engine (Chocolate
Doom lineage) plus a hardware port layer written for the ESP32-P4 and its AMOLED panel.

> **Heads up: this example is different from the rest of this repository.**
> Every other example here is PlatformIO + Arduino for the ESP32-S3. This one is a plain
> **ESP-IDF** project for the **ESP32-P4**. Do not try to open it with PlatformIO.

## What it does

- Full DOOM, not a raycaster demo: real WADs, real levels, save games, sound.
- 320x200 output scaled by the P4's **PPA** (Pixel Processing Accelerator) to **400x300**,
  which is an exact 4:3, centred on the 410x502 panel.
- **Double framebuffer**, so no tearing.
- The IWAD is loaded **whole into PSRAM** at boot, so the engine gets direct pointers into
  it and the microSD is only touched again to write save games.
- `doom1.wad` is embedded in the binary, so **it boots with no microSD inserted**.

## Hardware

| | |
|---|---|
| Board | Kode Dot v2 |
| MCU | ESP32-P4 (dual core, 32 MB PSRAM, 32 MB flash) |
| Display | CO5300 AMOLED, 410x502, MIPI-DSI |
| Coprocessor | ESP32-C5 over `esp_hosted`, powers the microSD and the audio amp |
| Input | 5-way D-pad and BOT button on a TCA6408 expander, TOP button on GPIO35 |

## You need a WAD before you can build

`doom1.wad` is **not** in this repository. It is 4.2 MB of game data and it does not belong
in a source repo, so you have to supply it yourself:

```bash
cp /path/to/doom1.wad main/doom1.wad
```

Without that file the build fails at the `EMBED_FILES` line in `main/CMakeLists.txt`.

Where to get one, in order of least hassle:

- **`doom1.wad`**, the DOOM 1 shareware episode. Freely redistributable as long as it is
  passed along complete and unmodified. Expected MD5 `f0cefca49926d00903cf57551d901abe`.
- **`doom.wad` / `doom2.wad`**, if you own the game (Steam, GOG, an old CD). 12 to 15 MB,
  they still fit whole in PSRAM.
- **Freedoom**, BSD-3 licensed and the only one you could legally ship inside a product.
  At 28.8 MB it exceeds the PSRAM budget, so it is read through stdio instead: level loads
  are slower, but it plays fine.

Commercial WADs are not redistributable. Do not commit yours.

## Build and flash

Requires **ESP-IDF v6.0 or newer** (developed against v6.0.1).

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p <PORT> flash monitor
```

The resulting binary is around 1.1 MB. `partitions_dev.csv` is a standalone development
partition table; it is what `idf.py flash` uses.

## Controls

| Control | In game | In menus |
|---|---|---|
| D-pad up / down | forward / back | move up / down |
| D-pad left / right | turn | change setting |
| **TOP** (upper button) | **fire** | **confirm** (also confirms "quit game") |
| **BOT** tap | **use / open doors** | back |
| **BOT** held 0.6 s | open menu (ESC) | close menu |

To quit: BOT for 0.6 s, then *Quit game*, then TOP. The board resets back to whatever it
was running before.

### Button traps that belong to the board, not to this app

If you build on top of this, these will bite you:

- **BOT + D-pad LEFT held together for about 1.4 s is a hardware reset** (via U12). That is
  precisely why BOT acts on release as a tap and why the menu gesture is only 0.6 s. If the
  board resets mid-game, this was it.
- **Holding BOT for a really long time (8 to 15 s) reaches the charger's QON pin.** Same
  reason.
- **TOP is GPIO35, a strapping pin.** It only matters if it is held down while plugging the
  board in; at runtime it is free.

## WADs on the microSD

The app looks in `/sd/doom/` and takes the first of:

```
doom2.wad -> doom.wad -> doom1.wad -> freedoom2.wad -> freedoom1.wad
```

If it finds no IWAD at all, it writes the embedded `doom1.wad` there and plays that. Save
games and `default.cfg` also live in `/sd/doom/`.

Anything up to 20 MB is loaded fully into PSRAM. Above that it falls back to stdio.

## How the port works

**`main/` is the port; everything under `components/doomgeneric/` is the upstream engine.**

| File | Job |
|---|---|
| `doom_display.c` | CO5300 bring-up over MIPI-DSI, no LVGL, two driver framebuffers |
| `doom_blit.c` | palette expansion and PPA scaling of the 320x200 frame |
| `doom_input.c` | polls the expander and GPIO35 at 60 Hz, maps to DOOM keys |
| `doom_wad.c` | brings up the C5 link, mounts the SD, loads the IWAD into PSRAM |
| `doom_wadfile_mem.c` | serves the in-PSRAM WAD to the engine's file layer |
| `doom_sound.c` | software mixer feeding the codec over I2S |
| `doom_haptic.c` | haptic feedback |

A few details worth knowing if you are porting DOOM to something else:

- The engine is built with `CMAP256`, so the framebuffer is 8-bit indexed and
  `I_FinishUpdate` becomes a per-line memcpy. The palette comes out of the engine already
  gamma-corrected.
- Both PPA scale factors land exactly on the accelerator's 1/16 quantisation
  (`320 * 20/16 = 400`, `200 * 24/16 = 300`), so there is no rounding anywhere.
- The flip is a real flip. Calling `draw_bitmap` with an *external* buffer makes the DSI
  driver copy into the framebuffer currently being scanned out, which is exactly the
  tearing you were trying to avoid. The zero-copy swap path is only taken when the pointer
  really is one of the driver's own framebuffers.
- Loading the IWAD into PSRAM instead of streaming it matters more than it sounds: the
  engine asks for direct pointers into the WAD constantly during play.
- The microSD runs in 1-bit mode on purpose. Some cards refuse to write in 4-bit, and 1-bit
  already sits at 97% of what the bus can do here.

The engine itself is vendored, not submoduled. **`components/doomgeneric/VENDORED.md`** lists
the exact upstream commit, what was removed, and every local patch with its reason. There
are three patches, all of them small.

## Dependencies

Managed components, resolved by `idf.py` from the Espressif registry:

- `espressif/esp_lcd_co5300`, `espressif/esp_io_expander_tca9554`
- `espressif/esp_hosted` **pinned to `==2.12.9`**, because it has to match the `c5_slave`
  firmware flashed on the board's C5. If RPC calls go quiet, unplug and replug the board
  before blaming anything else.
- `espressif/esp_wifi_remote`, `espressif/esp_codec_dev`

The C5 is what powers the microSD (IO26) and the audio amplifier (IO4), so with no C5 link
you get neither storage nor sound.

Codec volume starts at 60, which is the validated value. Going above 90 clips.

## Licence

**GPL-2.0-or-later.** The engine is doomgeneric, which descends from Chocolate Doom, and the
port layer is licensed to match. The `LICENSE` file in this directory governs this
subdirectory and must travel with the code if you redistribute it.

The 8x8 splash font (`main/font8x8_basic.h`) is public domain, by Daniel Hepper, based on
earlier work by Marcel Sondaar.

Game data is a separate matter from code: no WAD is included here, and commercial WADs must
not be redistributed.
