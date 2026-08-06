/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * doom_wad — enlace C5 (la SD la alimenta el C5), montaje de /sd y carga del
 * IWAD integro a PSRAM. La secuencia C5 + montaje reproduce la que ya estaba
 * validada en placa para esta board.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

#define DOOM_DIR       "/sd/doom"   /* saves: el motor crea /sd/doom/.savegame/ */
/* Hasta aqui se carga entero a RAM. Por encima (freedoom*, 28,8 MB) se queda en
 * stdio: no cabria junto a la zona de 8 MB y los framebuffers. */
#define DOOM_WAD_RAM_MAX (20u * 1024u * 1024u)

/* C5 arriba (CP_INIT) -> SD_PWR IO26=0 -> montar /sd -> crear /sd/doom[/saves]. */
esp_err_t doom_wad_boot(void);

/* Primera de: doom2 > doom > doom1 > freedoom2 > freedoom1 (.wad) en /sd/doom.
 * Devuelve ruta completa estatica o NULL. */
const char *doom_wad_find(void);

/* <= DOOM_WAD_RAM_MAX: a PSRAM y registra el backend mapeado. Mayor: no-op OK
 * (el motor tirara de stdio). */
esp_err_t doom_wad_load(const char *path);

const char *doom_wad_error(void);

/* Escribe el doom1.wad shareware EMBEBIDO en /sd/doom/ (primer arranque sin IWAD). */
esp_err_t doom_wad_seed_shareware(void);

/* SIN microSD: copia el doom1.wad EMBEBIDO de la flash a PSRAM y lo registra como
 * backend. Devuelve la ruta sintetica que hay que pasar en -iwad, o NULL si falla.
 * Se juega igual; lo unico que se pierde es guardar partida y el default.cfg. */
const char *doom_wad_load_embedded(void);

/* de doom_wadfile_mem.c */
void doom_wadfile_mem_register(const char *path, unsigned char *buf, unsigned int size);
int  doom_wadfile_mem_has(const char *path);
