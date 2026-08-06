/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_io_expander.h"

#include "doom_input.h"
#include "doom_display.h"
#include "doomkeys.h"

static const char *TAG = "doom_input";

#define BTN_TOP_GPIO      GPIO_NUM_35   /* directo al P4 (tambien strap: libre en runtime) */
#define POLL_MS           16            /* ~60 Hz; el rebote (~5 ms) cae entre muestras */
#define BOT_HOLD_MS       600           /* gesto de menu, MUY por debajo del 1,4 s del reset HW */

/* Los "soltar" sinteticos van RETRASADOS 2 ciclos (32 ms > 1 tic de 28,57 ms).
 * Review C1, confirmado en el motor: un pulsar+soltar encolados juntos entran en
 * el MISMO tic (i_input.c:322 postea ambos; BuildNewTic procesa eventos y LUEGO
 * construye el ticcmd) -> gamekeydown nunca se ve pulsado y USE jamas dispararia. */
#define SYNTH_RELEASE_CYCLES 2
#define MAX_PEND 4

/* TCA6408: P0=BOT P1=PAD_TOP P2=PAD_LEFT P3=PAD_BOTTOM P4=PAD_RIGHT.
 * Activo-bajo. P7 es DSP_PWR_EN: ampliar la mascara APAGA LA PANTALLA. */
#define EXP_BOT     IO_EXPANDER_PIN_NUM_0
#define EXP_UP      IO_EXPANDER_PIN_NUM_1
#define EXP_LEFT    IO_EXPANDER_PIN_NUM_2
#define EXP_DOWN    IO_EXPANDER_PIN_NUM_3
#define EXP_RIGHT   IO_EXPANDER_PIN_NUM_4
#define EXP_MASK    (EXP_BOT | EXP_UP | EXP_LEFT | EXP_DOWN | EXP_RIGHT)

enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_TOP, B_BOT, B_COUNT };

typedef struct {
    unsigned char key;
    uint8_t       pressed;
} key_evt_t;

typedef struct {
    unsigned char key;
    int8_t        cycles;   /* 0 = libre */
} pend_t;

static QueueHandle_t s_q;
static uint32_t s_i2c_errs;
static pend_t s_pend[MAX_PEND];

static void push(unsigned char key, uint8_t pressed)
{
    key_evt_t e = { .key = key, .pressed = pressed };
    /* Cola llena = se pierde el evento mas nuevo; con 32 de fondo y un juego
     * drenando cada tick no pasa en la practica. Jamas bloquear el muestreo. */
    (void)xQueueSend(s_q, &e, 0);
}

/* Pulsacion sintetica: down YA, up dentro de SYNTH_RELEASE_CYCLES ciclos. */
static void push_tap(unsigned char key)
{
    push(key, 1);
    for (int i = 0; i < MAX_PEND; i++) {
        if (s_pend[i].cycles == 0) {
            s_pend[i].key = key;
            s_pend[i].cycles = SYNTH_RELEASE_CYCLES;
            return;
        }
    }
    push(key, 0);   /* sin hueco (imposible en la practica): soltar ya */
}

static void pend_tick(void)
{
    for (int i = 0; i < MAX_PEND; i++) {
        if (s_pend[i].cycles > 0 && --s_pend[i].cycles == 0) {
            push(s_pend[i].key, 0);
        }
    }
}

static void pend_flush(void)
{
    for (int i = 0; i < MAX_PEND; i++) {
        if (s_pend[i].cycles > 0) {
            s_pend[i].cycles = 0;
            push(s_pend[i].key, 0);
        }
    }
}

static void input_task(void *arg)
{
    esp_io_expander_handle_t exp = doom_expander();
    bool raw[B_COUNT] = { 0 }, prev_raw[B_COUNT] = { 0 }, stable[B_COUNT] = { 0 };
    int64_t bot_t0 = 0;
    bool bot_esc_sent = false;

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_MS));
        pend_tick();

        /* TOP primero y SIEMPRE (GPIO directo): un fallo del expansor no puede
         * dejar sordo tambien al boton que no pasa por I2C (review M4). */
        raw[B_TOP] = (gpio_get_level(BTN_TOP_GPIO) == 0);

        uint32_t lv = 0;
        if (esp_io_expander_get_level(exp, EXP_MASK, &lv) == ESP_OK) {
            raw[B_UP]    = !(lv & EXP_UP);
            raw[B_DOWN]  = !(lv & EXP_DOWN);
            raw[B_LEFT]  = !(lv & EXP_LEFT);
            raw[B_RIGHT] = !(lv & EXP_RIGHT);
            raw[B_BOT]   = !(lv & EXP_BOT);
        } else {
            /* Fallo de bus != dedo levantado (leccion del tactil de kode_board):
             * se conserva la muestra anterior de los botones del expansor. */
            s_i2c_errs++;
            raw[B_UP] = prev_raw[B_UP]; raw[B_DOWN] = prev_raw[B_DOWN];
            raw[B_LEFT] = prev_raw[B_LEFT]; raw[B_RIGHT] = prev_raw[B_RIGHT];
            raw[B_BOT] = prev_raw[B_BOT];
        }

        for (int b = 0; b < B_COUNT; b++) {
            /* Anti-rebote: el estado solo cambia con DOS muestras seguidas de
             * acuerdo (peor caso 32 ms de latencia, imperceptible a 35 fps). */
            bool agreed = (raw[b] == prev_raw[b]);
            prev_raw[b] = raw[b];
            if (!agreed || raw[b] == stable[b]) {
                continue;
            }
            stable[b] = raw[b];
            bool dn = stable[b];

            switch (b) {
            /* Cruceta DERECHA (Victor, 05/08): con el blit en VERTICAL (rot 0) la
             * pantalla ya no esta girada, asi que la cruceta tampoco debe estarlo
             * -- arriba es adelante. Si se vuelve al apaisado (rot 270), hay que
             * volver a girar esto o los mandos quedan cruzados 90 grados. */
            case B_UP:    push(KEY_UPARROW, dn);    break;
            case B_DOWN:  push(KEY_DOWNARROW, dn);  break;
            case B_LEFT:  push(KEY_LEFTARROW, dn);  break;
            case B_RIGHT: push(KEY_RIGHTARROW, dn); break;
            case B_TOP:
                /* FIRE en juego y ENTER en menus: mandar ambas es inocuo (ENTER
                 * en juego no hace nada; FIRE en menus tampoco). El confirm de
                 * "salir" tambien es ENTER (key_menu_confirm, ver main.c). */
                push(KEY_FIRE, dn);
                push(KEY_ENTER, dn);
                break;
            case B_BOT:
                if (dn) {
                    pend_flush();   /* re-tap rapido: cerrar el tap anterior */
                    bot_t0 = esp_timer_get_time();
                    bot_esc_sent = false;
                } else if (!bot_esc_sent) {
                    /* tap al soltar: USE (en juego) + BACKSPACE (atras en menus,
                     * inocuo en juego). Soltar sintetico retrasado, ver arriba. */
                    push_tap(KEY_USE);
                    push_tap(KEY_BACKSPACE);
                }
                break;
            }
        }

        /* Gesto de menu: BOT mantenido 600 ms -> un ESC (y el suelte ya no usa). */
        if (stable[B_BOT] && !bot_esc_sent &&
            (esp_timer_get_time() - bot_t0) >= (int64_t)BOT_HOLD_MS * 1000) {
            bot_esc_sent = true;
            push_tap(KEY_ESCAPE);
        }
    }
}

esp_err_t doom_input_init(void)
{
    ESP_RETURN_ON_FALSE(doom_expander(), ESP_ERR_INVALID_STATE, TAG,
                        "doom_display_init() tiene que ir antes (expansor I2C)");

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_TOP_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,   /* refuerza el R38 externo de la placa */
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config BUTTON_TOP");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(doom_expander(), EXP_MASK, IO_EXPANDER_INPUT),
                        TAG, "expander dir");

    s_q = xQueueCreate(32, sizeof(key_evt_t));
    ESP_RETURN_ON_FALSE(s_q, ESP_ERR_NO_MEM, TAG, "sin memoria para la cola de teclas");

    /* Core 1: el juego vive en el 0. Prio 3 > tarea del juego, < mezclador de audio. */
    BaseType_t ok = xTaskCreatePinnedToCore(input_task, "doom_in", 3072, NULL, 3, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "sin memoria para la tarea de input");

    ESP_LOGI(TAG, "input listo: 60 Hz, cruceta+TOP+BOT (BOT hold %d ms = ESC)", BOT_HOLD_MS);
    return ESP_OK;
}

int doom_input_pop(int *pressed, unsigned char *key)
{
    key_evt_t e;
    if (!s_q || xQueueReceive(s_q, &e, 0) != pdTRUE) {
        return 0;
    }
    *pressed = e.pressed;
    *key = e.key;
    return 1;
}
