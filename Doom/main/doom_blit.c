/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/ppa.h"

#include "doom_blit.h"
#include "doom_display.h"

/* Del motor (CMAP256): la paleta ya gammada. struct color = bitfields b,g,r,a. */
#include "i_video.h"

static const char *TAG = "doom_blit";

#define BLIT_W 320
#define BLIT_H 200

/* VERTICAL (Victor, 05/08): el Kode se sostiene DE PIE, como se usa normalmente,
 * y DOOM sale como una banda apaisada centrada. Sin rotacion.
 *
 * Escalas EXACTAS en la cuantizacion de 1/16 del PPA:
 *   320 * 20/16 = 400 px de ancho (410 - 400 = 10 -> bordes de 5)
 *   200 * 24/16 = 300 px de alto  (502 - 300 = 202 -> bordes de 101)
 * 400:300 = 4:3 EXACTO, la relacion canonica de DOOM (320x200 estirado). Los dos
 * factores vuelven a caer clavados en los 1/16 del PPA, sin redondeos.
 *
 * (Apaisado, que es lo que habia antes: 25/16 y 30/16 -> 500x375 con rot 270.) */
#define SCALE_X (20.0f / 16.0f)
#define SCALE_Y (24.0f / 16.0f)
#define OUT_BLK_W 400   /* 320*20/16 (sin rotar, el eje X sigue siendo el X) */
#define OUT_BLK_H 300   /* 200*24/16 */
#define OUT_OFF_X ((DOOM_LCD_W - OUT_BLK_W) / 2)   /* 5   */
#define OUT_OFF_Y ((DOOM_LCD_H - OUT_BLK_H) / 2)   /* 101 */

/* Sin rotacion: la pantalla se mira de pie. */
#ifndef DOOM_ROT_ANGLE
#define DOOM_ROT_ANGLE PPA_SRM_ROTATION_ANGLE_0
#endif

static uint16_t *s_rgb;              /* 320x200 RGB565 en SRAM interna (128.000 B, 64-multiple) */
static uint16_t  s_lut[256];
static ppa_client_handle_t s_ppa;

esp_err_t doom_blit_init(void)
{
    /* SRAM interna a proposito: el PPA lee de aqui y solo escribe a PSRAM ->
     * minima contencion con el escaneo continuo del DSI (que tambien va a PSRAM). */
    s_rgb = heap_caps_aligned_calloc(64, 1, BLIT_W * BLIT_H * sizeof(uint16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_rgb) {
        /* MEDIDO EN PLACA (04/08): NO hay 128 kB internos libres — esp-hosted +
         * pilas se los comen. A PSRAM: mas contencion con el escaneo DSI, pero
         * el juego arranca. Si F0 mide el PPA lento, esta es la primera sospecha. */
        ESP_LOGW(TAG, "sin SRAM interna: staging a PSRAM (mas contencion con el DSI)");
        s_rgb = heap_caps_aligned_calloc(64, 1, BLIT_W * BLIT_H * sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_rgb, ESP_ERR_NO_MEM, TAG, "sin memoria para el staging RGB565");

    ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM };
    ESP_RETURN_ON_ERROR(ppa_register_client(&cfg, &s_ppa), TAG, "ppa_register_client");
    /* El angulo se IMPRIME, no se escribe a mano: antes ponia "rot90" fijo y
     * siguio diciendolo al pasar a vertical. Un log que miente es peor que no
     * tenerlo. El enum va 0/90/180/270 en orden, asi que el indice vale. */
    static const char *const ROT_NAME[] = { "rot0", "rot90", "rot180", "rot270" };
    ESP_LOGI(TAG, "blit listo: 320x200 -> %s + %dx%d @ (%d,%d)",
             ROT_NAME[DOOM_ROT_ANGLE], OUT_BLK_W, OUT_BLK_H, OUT_OFF_X, OUT_OFF_Y);
    return ESP_OK;
}

static void rebuild_lut(void)
{
    /* colors[] es del motor (i_video.c, no-static solo en CMAP256). 256 entradas
     * por frame = nada; asi los cambios de paleta (dano/berserk) salen gratis. */
    for (int i = 0; i < 256; i++) {
        s_lut[i] = (uint16_t)(((colors[i].r & 0xF8) << 8) |
                              ((colors[i].g & 0xFC) << 3) |
                              ((colors[i].b) >> 3));
    }
}

void doom_blit_frame(const uint8_t *frame)
{
    rebuild_lut();

    const uint8_t *in = frame;
    uint16_t *out = s_rgb;
    for (int i = 0; i < BLIT_W * BLIT_H; i++) {
        *out++ = s_lut[*in++];
    }

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = s_rgb,
            .pic_w = BLIT_W,
            .pic_h = BLIT_H,
            .block_w = BLIT_W,
            .block_h = BLIT_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = doom_display_stage(),
            .buffer_size = DOOM_STAGE_BYTES,
            .pic_w = DOOM_LCD_W,
            .pic_h = DOOM_LCD_H,
            .block_offset_x = OUT_OFF_X,
            .block_offset_y = OUT_OFF_Y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = DOOM_ROT_ANGLE,
        .scale_x = SCALE_X,
        .scale_y = SCALE_Y,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t e = ppa_do_scale_rotate_mirror(s_ppa, &op);
    if (e != ESP_OK) {
        /* Que se vea en el log ANTES de plantearse el fallback CPU (F0 decide). */
        ESP_LOGE(TAG, "ppa SRM: %s", esp_err_to_name(e));
        return;
    }

    doom_display_present();
}
