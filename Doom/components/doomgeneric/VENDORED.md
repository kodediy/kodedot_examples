# doomgeneric vendored

- Origen: https://github.com/ozkl/doomgeneric
- Commit: `dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284` (clonado 2026-08-04, --depth 1)
- Licencia: GPL-2.0 (ver LICENSE en la raíz de la app)

## Quitado del upstream (backends de plataforma, no compilables aquí)

`doomgeneric_allegro.c`, `doomgeneric_emscripten.c`, `doomgeneric_linuxvt.c`,
`doomgeneric_sdl.c`, `doomgeneric_soso.c`, `doomgeneric_sosox.c`, `doomgeneric_win.c`,
`doomgeneric_xlib.c`, `i_allegromusic.c`, `i_allegrosound.c`, `i_sdlmusic.c`,
`i_sdlsound.c`, `*.vcxproj*`. Quedan 83 `.c`.

## Build

`CMAP256` (framebuffer 8-bit indexado, `pixel_t = uint8_t`) + `DOOMGENERIC_RESX=320`
`DOOMGENERIC_RESY=200` → `fb_scaling=1` → `I_FinishUpdate` = memcpy por línea + `DG_DrawFrame()`.
La paleta ya sale exportada de serie: `extern struct color colors[256]` (i_video.h, solo CMAP256;
bitfields orden b,g,r,a). `-w` para el código vendor; NUESTROS ficheros compilan sin warnings.

## Parches locales (mantener esta lista al día)

| Fichero | Cambio | Motivo |
|---|---|---|
| `w_file.c` | `W_OpenFile` prueba `psram_wad_file` (extern, vive en `main/doom_wadfile_mem.c`) antes del camino stdio; NULL = sigue igual | WAD íntegro en PSRAM con `mapped` (cero copias, w_wad.c:399) sin romper rutas stdio (saves, PWADs) |
| `i_sound.c` | quitado el `#include <SDL_mixer.h>` (bajo FEATURE_SOUND) | include huérfano: el fichero no usa ningún símbolo SDL, solo lo menciona en comentarios |
| `d_iwad.c` | `D_FindWADByName` pregunta a `doom_wadfile_mem_has()` (extern, `main/doom_wadfile_mem.c`) **antes** del `M_FileExists` | Arranque **sin microSD**: el IWAD va embebido y se sirve de PSRAM, así que no existe en ningún FS y `M_FileExists` (que es un `fopen`) lo daba por perdido → `I_Error("IWAD file not found!")` |

## Decididamente NO parcheado

- `I_Quit`/`I_Error` → `exit()`: se captura con `atexit(esp_restart)` en `main.c` (contrato
  Kode OS: reset = rollback al OS).
- `DEFAULT_RAM 6 MiB`: la zona real se pasa con `-mb 8` en argv.
- `SAVEGAMESIZE`: el límite solo se aplica si `vanilla_savegame_limit` (g_game.c:1653);
  se decide en A7 si se desactiva por config o se amplía la constante.
