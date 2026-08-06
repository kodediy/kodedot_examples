/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * doom_blit — del frame 8-bit indexado de DOOM al panel:
 *   LUT paleta->RGB565 (CPU, staging en SRAM interna)
 *   -> PPA SRM rot90 + escala sx=25/16 sy=30/16 -> 500x375 (4:3 EXACTO)
 *   -> centrado en el staging del display -> present().
 * La paleta se relee de colors[] del motor en cada frame (256 entradas, gratis):
 * cubre los flashes de dano/objetos sin necesitar gancho de cambio.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

esp_err_t doom_blit_init(void);

/* frame = DG_ScreenBuffer (320x200, 1 byte/pixel). Bloquea hasta present. */
void doom_blit_frame(const uint8_t *frame);
