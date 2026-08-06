/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * doom_haptic — vibracion con el AW86223 (I2C 0x58, placa de antena).
 *
 * Secuencia RTP calcada de 12-POWER/power_lab (validada en placa), con sus dos
 * arreglos: el bucle del FIFO va ACOTADO (sin cota, un FIFO que deja de drenar
 * cuelga al llamante para siempre, martillea el I2C y DEJA LA LRA SONANDO) y el
 * STOP se escribe pase lo que pase.
 *
 * Aqui las peticiones van por COLA a una tarea propia: aw_buzz_ms() bloquea
 * decenas de ms y el juego no puede pararse por un zumbido. La cola descarta si
 * esta llena (mejor perder un buzz que retrasar un frame) — eso ES lo correcto
 * aqui, al reves que en power_lab, donde descartar falseaba una medida.
 */
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"

#include "doom_haptic.h"
#include "doom_display.h"

static const char *TAG = "doom_haptic";

#define AW_ADDR         0x58
#define AW_REG_ID       0x00
#define AW_REG_SYSST    0x01
#define AW_REG_PLAYCFG2 0x07
#define AW_REG_PLAYCFG3 0x08
#define AW_REG_PLAYCFG4 0x09
#define AW_REG_RTPDATA  0x32
#define AW_PLAYMODE_RTP 0x01
#define AW_GO           0x01
#define AW_STOP         0x02
#define AW_FF_AFS       0x08
#define AW_GAIN         0x80
#define AW_SR_HZ        12000
#define LRA_F0_HZ       235      /* LRA0832BC: F0 fijo, la intensidad es la amplitud */
#define HAP_MAX_MS      120
#define HAP_MAX_SAMPLES (AW_SR_HZ * HAP_MAX_MS / 1000)

typedef struct { uint16_t ms; uint8_t amp; } buzz_req_t;

static i2c_master_dev_handle_t s_dev;
static QueueHandle_t s_q;
static int8_t s_pcm[HAP_MAX_SAMPLES];
static bool s_ok;

static esp_err_t aw_w(uint8_t r, uint8_t v)
{
    uint8_t b[2] = { r, v };
    return i2c_master_transmit(s_dev, b, 2, 1000);
}

static esp_err_t aw_r(uint8_t r, uint8_t *v)
{
    return i2c_master_transmit_receive(s_dev, &r, 1, v, 1, 1000);
}

static esp_err_t aw_fifo(const int8_t *pcm, int n)
{
    uint8_t buf[1 + 64];
    /* Reproducir n muestras tarda n/SR s; pasado eso + 500 ms es un fallo. */
    int64_t deadline = esp_timer_get_time() + ((int64_t)n * 1000 / AW_SR_HZ + 500) * 1000;
    while (n > 0) {
        if (esp_timer_get_time() > deadline) return ESP_ERR_TIMEOUT;
        uint8_t st = 0;
        /* Un status que FALLA no es permiso para escribir: "no se" = esperar. */
        if (aw_r(AW_REG_SYSST, &st) != ESP_OK || (st & AW_FF_AFS)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        int c = n > 64 ? 64 : n;
        buf[0] = AW_REG_RTPDATA;
        memcpy(&buf[1], pcm, c);
        if (i2c_master_transmit(s_dev, buf, 1 + c, 1000) != ESP_OK) return ESP_FAIL;
        pcm += c; n -= c;
    }
    return ESP_OK;
}

static void buzz(int ms, int amp)
{
    if (ms <= 0) return;
    if (ms > HAP_MAX_MS) ms = HAP_MAX_MS;
    int n = AW_SR_HZ * ms / 1000;
    for (int i = 0; i < n; i++) {
        double ph = 2.0 * M_PI * LRA_F0_HZ * i / AW_SR_HZ;
        s_pcm[i] = (int8_t)lround((amp / 255.0) * 127.0 * sin(ph));
    }
    aw_w(AW_REG_PLAYCFG2, AW_GAIN);
    aw_w(AW_REG_PLAYCFG3, AW_PLAYMODE_RTP);
    aw_w(AW_REG_PLAYCFG4, AW_GO);
    vTaskDelay(pdMS_TO_TICKS(2));
    esp_err_t e = aw_fifo(s_pcm, n);
    aw_w(AW_REG_PLAYCFG4, AW_STOP);   /* SIEMPRE: si no, la LRA se queda sonando */
    if (e != ESP_OK) ESP_LOGE(TAG, "FIFO %s -> LRA parada a la fuerza", esp_err_to_name(e));
}

static void haptic_task(void *arg)
{
    buzz_req_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) == pdTRUE) {
            buzz(r.ms, r.amp);
        }
    }
}

void doom_haptic_buzz(int ms, int amp)
{
    if (!s_ok) return;
    buzz_req_t r = { .ms = (uint16_t)ms, .amp = (uint8_t)amp };
    (void)xQueueSend(s_q, &r, 0);   /* lleno -> se descarta; jamas frenar el juego */
}

esp_err_t doom_haptic_init(void)
{
    if (i2c_master_probe(doom_i2c(), AW_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "AW86223 sin ACK @0x%02X (FPC de antena?): sin vibracion", AW_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AW_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(doom_i2c(), &dc, &s_dev), TAG, "add dev");

    uint8_t id = 0;
    aw_r(AW_REG_ID, &id);   /* informativo: el ID varia por lote (0x01/0xB3/0xBB) */

    s_q = xQueueCreate(4, sizeof(buzz_req_t));
    ESP_RETURN_ON_FALSE(s_q, ESP_ERR_NO_MEM, TAG, "sin memoria para la cola");
    /* Core 1, prio 2: por debajo del input y del mezclador; nunca pisa al juego. */
    BaseType_t ok = xTaskCreatePinnedToCore(haptic_task, "doom_hap", 3072, NULL, 2, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "sin memoria para la tarea");

    s_ok = true;
    ESP_LOGI(TAG, "vibracion lista (AW86223 id=0x%02X, F0 %d Hz)", id, LRA_F0_HZ);
    return ESP_OK;
}
