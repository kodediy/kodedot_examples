/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * doom_haptic — vibracion (AW86223 @0x58, en la placa de antena).
 * Necesita doom_display_init() hecho (usa su bus I2C).
 * Tolerante a fallo: sin FPC de antena, el juego va igual y sin vibrar.
 */
#pragma once

#include "esp_err.h"

esp_err_t doom_haptic_init(void);

/* ms 1..120, amp 0..255. NO bloquea (encola; si esta llena, descarta). */
void doom_haptic_buzz(int ms, int amp);
