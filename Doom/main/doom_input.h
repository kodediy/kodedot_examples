/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * doom_input — cruceta (TCA6408 P1..P4) + BUTTON_TOP (GPIO35) + BUTTON_BOT (P0)
 * muestreados a 60 Hz en una tarea del core 1; los flancos se convierten en
 * eventos de tecla DOOM y los drena DG_GetKey.
 *
 * Mapeo:            cruceta = flechas · TOP = FIRE (+ENTER en menus)
 *                   BOT tap = USE · BOT >=600 ms = ESC (menu de DOOM)
 * ⚠️ BOT jamas se exige sostenido: BOT es QON del cargador y BOT+PAD_LEFT
 *    mantenidos ~1,4 s es el RESET por hardware (U12). El gesto de 600 ms se
 *    suelta muy por debajo del umbral.
 */
#pragma once

#include "esp_err.h"

/* Necesita doom_display_init() hecho (usa su expansor I2C). */
esp_err_t doom_input_init(void);

/* Contrato de DG_GetKey: devuelve 1 y rellena si habia evento, 0 si no. */
int doom_input_pop(int *pressed, unsigned char *key);
