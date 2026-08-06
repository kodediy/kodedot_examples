/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * psram_wad_file — backend w_file del motor sobre el WAD precargado en PSRAM.
 * Con wad.mapped != NULL, W_CacheLumpNum() sirve PUNTEROS DIRECTOS al buffer
 * (w_wad.c:399): cero copias y cero I/O de SD durante el juego.
 *
 * Solo hay un WAD precargado a la vez (el IWAD). Los PWADs, si algun dia se
 * cargan, iran por el backend stdio de siempre.
 */
#include <string.h>

#include "w_file.h"

static wad_file_t s_wad;             /* estatico: no depende de Z_Malloc */
static const char *s_path;           /* path registrado (el que ira en -iwad) */

static wad_file_t *W_Psram_OpenFile(char *path)
{
    if (!s_path || !path || strcmp(path, s_path) != 0) {
        return NULL;   /* no es el precargado: que lo abra el stdc */
    }
    return &s_wad;
}

static void W_Psram_CloseFile(wad_file_t *wad)
{
    (void)wad;   /* el buffer vive lo que vive el juego */
}

static size_t W_Psram_Read(wad_file_t *wad, unsigned int offset,
                           void *buffer, size_t buffer_len)
{
    if (offset >= wad->length) {
        return 0;
    }
    size_t n = wad->length - offset;
    if (n > buffer_len) {
        n = buffer_len;
    }
    memcpy(buffer, wad->mapped + offset, n);
    return n;
}

wad_file_class_t psram_wad_file = {
    W_Psram_OpenFile,
    W_Psram_CloseFile,
    W_Psram_Read,
};

/* Para el arranque SIN microSD: el motor resuelve el -iwad con M_FileExists(),
 * que hace fopen() — y un WAD que solo vive en PSRAM no existe en ningun sistema
 * de ficheros. d_iwad.c pregunta aqui antes de darlo por perdido. */
int doom_wadfile_mem_has(const char *path)
{
    return (s_path && path && strcmp(path, s_path) == 0) ? 1 : 0;
}

void doom_wadfile_mem_register(const char *path, unsigned char *buf, unsigned int size)
{
    s_path = path;
    s_wad.file_class = &psram_wad_file;
    s_wad.mapped = buf;
    s_wad.length = size;
}
