/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * doom_display — panel CO5300 410x502 por MIPI-DSI, SIN LVGL y con num_fbs=2.
 * Bring-up igual al del BSP de la board (validado en placa) salvo en DOS cosas,
 * ambas deliberadas: doble framebuffer del driver y ninguna tarea de UI.
 *
 * Flip (corregido tras review): draw_bitmap con un buffer EXTERNO copia al FB
 * ESCANEADO (esp_lcd_panel_dpi.c:600-649) — eso seria tearing. El camino sin
 * copia + conmutado solo se toma si el puntero ES un FB del driver. Por eso
 * present() = memcpy staging->FB trasero + draw_bitmap(FB trasero) = flip real.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#define DOOM_LCD_W 410
#define DOOM_LCD_H 502
/* Tamano del staging redondeado a 64 (el PPA exige buffer_size alineado a linea
 * de cache; 410*502*2 = 411.640 no lo es -> 411.648). */
#define DOOM_STAGE_BYTES (((DOOM_LCD_W * DOOM_LCD_H * 2) + 63) & ~63)

esp_err_t doom_display_init(void);

/* Staging RGB565 410x502 en PSRAM (64-alineado): aqui pintan el PPA y el splash. */
uint16_t *doom_display_stage(void);

/* Copia el staging al FB trasero (DMA2D) y espera el fin de la copia. */
esp_err_t doom_display_present(void);

/* 0-100. AMOLED: sin GPIO de backlight, es el comando 0x51 del panel. */
esp_err_t doom_display_brightness(uint8_t pct);

/* Bus I2C y expansor compartidos (los reusa doom_input para la cruceta). */
i2c_master_bus_handle_t doom_i2c(void);
esp_io_expander_handle_t doom_expander(void);
