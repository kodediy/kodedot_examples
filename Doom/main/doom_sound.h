/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * doom_sound — SFX del DOOM por el ES8311 + NS4150B del Kode.
 *
 *   lumps DMX 11.025 Hz / 8-bit / mono
 *     -> mezclador propio de 8 canales (tarea en el core 1)
 *     -> 22.050 Hz / 16-bit / estereo -> esp_codec_dev (I2S) -> altavoz
 *
 * hw_gain VA CORRECTO A PROPOSITO (pa_voltage=4.2, no el 5.0 del ejemplo): los
 * valores stock meten +3,5 dB de boost digital al volumen alto y recortan
 * DENTRO del codec [project_kode_audio_distorsion]. Volumen validado: 60.
 *
 * El PA (NS4150B) se habilita por RPC al C5 (PA_CTRL = C5 IO4, polaridad "por
 * confirmar" -> F0). Exige el enlace C5 arriba: llamar DESPUES de doom_wad_boot().
 * Si el audio falla, el juego sigue MUDO (nunca se aborta por sonido).
 */
#pragma once

#include "esp_err.h"

/* Tolerante a fallo: ESP_OK o el motivo; en fallo el juego va sin sonido. */
esp_err_t doom_sound_init(void);
