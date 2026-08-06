/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * app_doom — DOOM en el Kode Dot. Orquestacion del arranque:
 *
 *   display (splash YA visible) -> C5 -> SD -> IWAD a PSRAM -> input -> juego
 *
 * Esta app JAMAS llama a esp_ota_mark_app_valid_cancel_rollback() ni a
 * esp_ota_set_boot_partition(): asi, si se arranca desde una particion OTA,
 * salir = reset y el rollback de serie devuelve a la app anterior. El exit()
 * del motor (I_Quit desde el menu de DOOM, I_Error en un fallo) se captura
 * con atexit().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doom_display.h"
#include "doom_input.h"
#include "doom_wad.h"
#include "doom_sound.h"
#include "doom_haptic.h"

#include "font8x8_basic.h"   /* dominio publico (Daniel Hepper / Marcel Sondaar) */

static const char *TAG = "app_doom";

/* El confirm del "¿seguro que quieres salir?" es 'y' por defecto y este cacharro
 * no tiene teclado: se re-liga a ENTER (= BUTTON_TOP). Es variable de config del
 * motor (m_controls.c:157), se pisa DESPUES de doomgeneric_Create. */
extern int key_menu_confirm;
/* FreeDoom peta el limite vanilla de savegame (g_game.c:1653) -> fuera (review I4). */
extern int vanilla_savegame_limit;

/* ===========================================================================
 *  Splash / pantallas de texto sin LVGL (fuente 8x8, escala x3)
 * =========================================================================== */
#define GLYPH_SCALE 2
#define GLYPH_W     (8 * GLYPH_SCALE)

#define COL_BLACK  0x0000
#define COL_WHITE  0xFFFF
#define COL_YELLOW 0xFD60   /* Bold Yellow #FFAC00 en RGB565 */

static void draw_text(uint16_t *fb, const char *s, int cx, int y, uint16_t color)
{
    int len = (int)strlen(s);
    int x0 = cx - (len * GLYPH_W) / 2;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c > 127) c = '?';
        for (int gy = 0; gy < 8; gy++) {
            uint8_t row = (uint8_t)font8x8_basic[c][gy];
            for (int gx = 0; gx < 8; gx++) {
                if (!(row & (1 << gx))) continue;
                for (int sy = 0; sy < GLYPH_SCALE; sy++) {
                    for (int sx = 0; sx < GLYPH_SCALE; sx++) {
                        int px = x0 + i * GLYPH_W + gx * GLYPH_SCALE + sx;
                        int py = y + gy * GLYPH_SCALE + sy;
                        if (px >= 0 && px < DOOM_LCD_W && py >= 0 && py < DOOM_LCD_H) {
                            fb[py * DOOM_LCD_W + px] = color;
                        }
                    }
                }
            }
        }
    }
}

/* lines[] separadas por '\n' en un solo string; la primera linea va en amarillo. */
static void splash(const char *text)
{
    uint16_t *fb = doom_display_stage();
    memset(fb, 0, DOOM_STAGE_BYTES);

    int nlines = 1;
    for (const char *p = text; *p; p++) if (*p == '\n') nlines++;
    int line_h = 8 * GLYPH_SCALE + 10;
    int y = (DOOM_LCD_H - nlines * line_h) / 2;

    char buf[64];
    const char *p = text;
    int line = 0;
    while (*p) {
        size_t n = strcspn(p, "\n");
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, p, n); buf[n] = '\0';
        draw_text(fb, buf, DOOM_LCD_W / 2, y + line * line_h,
                  (line == 0) ? COL_YELLOW : COL_WHITE);
        p += n + (p[n] == '\n' ? 1 : 0);
        line++;
    }
    doom_display_present();
}

/* Error terminal: mensaje + cualquier boton = volver (reset -> rollback al OS). */
static void fail_screen(const char *msg)
{
    char full[256];
    snprintf(full, sizeof(full), "%s\n\nPULSA UN BOTON\nPARA VOLVER", msg);
    splash(full);
    ESP_LOGE(TAG, "parado: %s", msg);
    /* 30 s y reset solo: si el input tampoco levanto, que no se quede preso (M2). */
    for (int i = 0; i < 600; i++) {
        int pressed; unsigned char key;
        if (doom_input_pop(&pressed, &key) && pressed) {
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_restart();
}

/* ===========================================================================
 *  Juego
 * =========================================================================== */
static void game_task(void *arg)
{
    char *wad_path = arg;

    /* Config + saves relativos -> /sd/doom (default.cfg, .savegame/). Este
     * Chocolate no tiene -savedir; GetDefaultConfigDir() devuelve "." y con el
     * chdir todo aterriza donde toca sin parchear el motor. */
    if (chdir(DOOM_DIR) != 0) {
        ESP_LOGW(TAG, "chdir %s fallo: config/saves no persistiran", DOOM_DIR);
    }

    char *argv[] = { "doom", "-iwad", wad_path, "-mb", "8" };
    doomgeneric_Create(5, argv);

    key_menu_confirm = KEY_ENTER;   /* TOP confirma el quit (ver arriba) */
    vanilla_savegame_limit = 0;

    for (;;) {
        doomgeneric_Tick();
    }
}

void app_main(void)
{
    /* exit(0) de I_Quit / exit(-1) de I_Error -> reset limpio -> rollback al OS.
     * Registrado ANTES de nada que pueda petar. */
    atexit(esp_restart);

    ESP_ERROR_CHECK(doom_display_init());
    splash("DOOM\ncargando...");

    /* Input pronto: las pantallas de error ya se navegan con botones. */
    ESP_ERROR_CHECK(doom_input_init());

    /* SIN microSD se juega igual: el doom1.wad va embebido en la flash, asi que
     * la tarjeta solo hace falta para PERSISTIR (partidas y default.cfg). Antes
     * esto era una pantalla de error; no lo merece. */
    const char *wad;
    if (doom_wad_boot() != ESP_OK) {
        splash("DOOM\nsin tarjeta SD:\nno se guardaran partidas");
        wad = doom_wad_load_embedded();
        if (!wad) {
            fail_screen(doom_wad_error());
        }
    } else {
        wad = doom_wad_find();
        if (!wad) {
            splash("DOOM\nprimer arranque:\nsembrando doom1.wad...");
            doom_wad_seed_shareware();
            wad = doom_wad_find();
            if (!wad) {
                fail_screen(doom_wad_error());
            }
        }

        splash("DOOM\nleyendo WAD...");
        if (doom_wad_load(wad) != ESP_OK) {
            fail_screen(doom_wad_error() ? doom_wad_error() : "fallo cargando el WAD");
        }
    }

    /* Tolerantes a fallo: sin codec el juego va mudo, sin antena no vibra;
     * ninguno de los dos justifica una pantalla de error. */
    doom_sound_init();
    doom_haptic_init();

    /* Core 0 para el juego (input ya vive en el core 1). PILA EN PSRAM, MEDIDO en
     * placa 04/08: con el audio ya funcionando quedan ~18 kB de RAM INTERNA — sus
     * buffers DMA se la comen — y 48 kB internos no caben ni de lejos. La tarea no
     * corre en ISR ni con la cache apagada, asi que PSRAM es legal aqui
     * (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y). */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(game_task, "doom", 48 * 1024,
                                                    (void *)wad, 5, NULL, 0,
                                                    MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        fail_screen("sin memoria para la tarea del juego");
    }
    ESP_LOGI(TAG, "DOOM arrancando con %s", wad);
}
