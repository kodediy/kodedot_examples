/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * doomgeneric_kode.c — las 5 funciones DG_* del port al Kode Dot.
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "doomgeneric.h"
#include "doom_blit.h"
#include "doom_input.h"

void DG_Init(void)
{
    /* El display ya esta arriba (main.c lo levanta ANTES para el splash). */
    ESP_ERROR_CHECK(doom_blit_init());
}

void DG_DrawFrame(void)
{
    doom_blit_frame((const uint8_t *)DG_ScreenBuffer);
}

void DG_SleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));   /* CONFIG_FREERTOS_HZ=1000 -> 1 ms reales */
}

uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    return doom_input_pop(pressed, key);
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}
