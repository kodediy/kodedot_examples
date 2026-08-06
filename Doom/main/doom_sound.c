/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_hosted_cp_gpio.h"

#include "doom_sound.h"
#include "doom_display.h"
#include "doom_haptic.h"

/* Del motor */
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

static const char *TAG = "doom_sound";

/* i_sound.c (FEATURE_SOUND) liga estas dos a config; vivian en el i_sdlsound.c
 * que no compilamos. Defaults de Chocolate; aqui no se usan para nada. */
int   use_libsamplerate  = 0;
float libsamplerate_scale = 0.65f;

/* I2S del Kode (05-AUDIO/es8311, validado): MCLK 33, SCLK 34, WS 37, DOUT 38. */
#define I2S_MCLK_IO 33
#define I2S_BCLK_IO 34
#define I2S_WS_IO   37
#define I2S_DOUT_IO 38

#define OUT_RATE     22050            /* lumps a 11.025 -> x2 por duplicado */
#define TIC_FRAMES   (OUT_RATE / 35)  /* 630 frames = 1 tic de juego */
#define NUM_CH       8
#define VOL_START    60               /* el validado en placa; >90 recorta */

#define C5_PA_CTRL   4                /* NS4150B enable = C5 IO4 (polaridad F0) */

typedef struct {
    const uint8_t *data;   /* payload 8-bit unsigned (ya sin cabecera DMX) */
    uint32_t       len;
    uint32_t       pos;    /* en muestras FUENTE (11.025 Hz) */
    int            l_mul;  /* volumen ya paneado, 0..127 por lado */
    int            r_mul;
    volatile uint32_t gen; /* contador de re-armado: el mixer descarta su pasada
                              si el canal fue re-armado a mitad (review I3) */
    volatile bool  on;
} channel_t;

static channel_t            s_ch[NUM_CH];
static esp_codec_dev_handle_t s_dev;
static bool                 s_ready;
static bool                 s_prefix;

/* ---------------------------------------------------------------- mezclador */

static void set_pan(channel_t *c, int vol, int sep)
{
    /* sep 0..254, 128 = centro (convencion s_sound.c). Paneo lineal clasico. */
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    c->l_mul = vol * (254 - sep) / 254;
    c->r_mul = vol * sep / 254;
}

static void mixer_task(void *arg)
{
    static int16_t out[TIC_FRAMES * 2];   /* estereo intercalado */
    static int32_t acc[TIC_FRAMES * 2];

    for (;;) {
        memset(acc, 0, sizeof(acc));
        for (int n = 0; n < NUM_CH; n++) {
            channel_t *c = &s_ch[n];
            if (!c->on) continue;
            /* Snapshot local: si snd_start (core 0) re-arma el canal a mitad de
             * esta pasada, la mezcla usa datos coherentes VIEJOS y el write-back
             * se descarta al ver el gen cambiado. Peor caso: un tic del sonido
             * anterior — jamas indices con data/len descasados (review I3). */
            uint32_t gen0 = c->gen;
            const uint8_t *data = c->data;
            uint32_t len = c->len;
            uint32_t pos = c->pos;
            int l_mul = c->l_mul, r_mul = c->r_mul;
            bool ended = false;
            for (int f = 0; f < TIC_FRAMES; f++) {
                uint32_t src = pos + (uint32_t)(f >> 1);   /* x2: cada muestra, dos veces */
                if (src >= len) { ended = true; break; }
                int s = ((int)data[src] - 128) << 8;       /* u8 -> s16 */
                acc[f * 2]     += s * l_mul / 127;
                acc[f * 2 + 1] += s * r_mul / 127;
            }
            if (c->gen == gen0) {
                c->pos = pos + TIC_FRAMES / 2;
                if (ended || c->pos >= len) c->on = false;
            }
        }
        for (int i = 0; i < TIC_FRAMES * 2; i++) {
            int v = acc[i];
            if (v >  32767) v =  32767;    /* saturar, no envolver */
            if (v < -32768) v = -32768;
            out[i] = (int16_t)v;
        }
        /* Bloquea contra el DMA del I2S: el propio reloj de audio marca el paso. */
        esp_codec_dev_write(s_dev, out, sizeof(out));
    }
}

/* ------------------------------------------------- modulo de SFX del motor */

static boolean snd_init(boolean use_sfx_prefix)
{
    s_prefix = use_sfx_prefix;
    return s_ready;
}

static void snd_shutdown(void)
{
    if (s_ready) esp_hosted_cp_gpio_set_level(C5_PA_CTRL, 0);   /* silenciar PA */
}

static int snd_get_lump(sfxinfo_t *sfx)
{
    char name[16];
    if (sfx->link) sfx = sfx->link;
    if (s_prefix) snprintf(name, sizeof(name), "ds%s", sfx->name);
    else          snprintf(name, sizeof(name), "%s", sfx->name);
    return W_GetNumForName(name);
}

static void snd_update(void) { }   /* la tarea mezcla sola */

static void snd_update_params(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NUM_CH) return;
    set_pan(&s_ch[channel], vol, sep);
}

/* VIBRACION enganchada al SONIDO, a proposito: el motor ya nos dice aqui, por su
 * nombre, exactamente que ha pasado (disparo, dolor, explosion) — asi no hay que
 * parchear p_pspr.c ni p_inter.c. Va ANTES del check de s_ready para que el Kode
 * vibre aunque el codec no haya levantado. */
static void haptic_for(const char *name)
{
    static const struct { const char *n; uint8_t ms; uint8_t amp; } MAP[] = {
        { "pistol", 12, 110 }, { "shotgn", 22, 200 }, { "dshtgn", 30, 255 },
        { "plasma",  8,  90 }, { "bfg",    45, 255 }, { "rlaunc", 28, 230 },
        { "sawful",  0,   0 }, { "punch",  14, 150 },
        { "barexp", 40, 255 },                       /* explosion (barril/cohete) */
        { "plpain", 25, 190 }, { "oof",    18, 140 },/* al jugador le duele */
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        if (strcmp(name, MAP[i].n) == 0) {
            if (MAP[i].ms) doom_haptic_buzz(MAP[i].ms, MAP[i].amp);
            return;
        }
    }
}

static int snd_start(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    haptic_for(sfxinfo->name);

    if (!s_ready || channel < 0 || channel >= NUM_CH) return -1;

    int lump = sfxinfo->lumpnum;
    /* WAD en PSRAM mapeado => esto es un puntero directo, sin copia ni I/O. */
    const uint8_t *raw = W_CacheLumpNum(lump, PU_STATIC);
    unsigned int  rawlen = W_LumpLength(lump);

    /* Cabecera DMX: u16 formato(3), u16 rate, u32 muestras; el payload lleva 16
     * bytes de guarda delante y detras (formato 1.9). Parseo con bounds. */
    if (rawlen < 8 || raw[0] != 0x03 || raw[1] != 0x00) return -1;
    uint32_t n = raw[4] | (raw[5] << 8) | (raw[6] << 16) | ((uint32_t)raw[7] << 24);
    const uint8_t *payload = raw + 8;
    if (n > rawlen - 8) n = rawlen - 8;
    if (n > 32) { payload += 16; n -= 32; }   /* fuera las guardas */

    channel_t *c = &s_ch[channel];
    c->on = false;                  /* parar antes de re-armar (misma ranura) */
    c->gen++;                       /* invalida cualquier pasada del mixer en vuelo */
    c->data = payload;
    c->len = n;
    c->pos = 0;
    set_pan(c, vol, sep);
    c->on = true;
    return channel;
}

static void snd_stop(int channel)
{
    if (channel >= 0 && channel < NUM_CH) s_ch[channel].on = false;
}

static boolean snd_playing(int channel)
{
    return channel >= 0 && channel < NUM_CH && s_ch[channel].on;
}

static void snd_cache(sfxinfo_t *sounds, int num_sounds)
{
    (void)sounds; (void)num_sounds;   /* el WAD ya vive en PSRAM: nada que precachear */
}

static snddevice_t sfx_devices[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    sfx_devices,
    sizeof(sfx_devices) / sizeof(sfx_devices[0]),
    snd_init,
    snd_shutdown,
    snd_get_lump,
    snd_update,
    snd_update_params,
    snd_start,
    snd_stop,
    snd_playing,
    snd_cache,
};

/* ------------------------------ musica: modulo NULO (la OPL llega en fase 3) */

static boolean mus_init(void) { return true; }
static void    mus_noop(void) { }
static void    mus_setvol(int v) { (void)v; }
static void   *mus_register(void *data, int len) { (void)data; (void)len; return NULL; }
static void    mus_unregister(void *h) { (void)h; }
static void    mus_play(void *h, boolean loop) { (void)h; (void)loop; }
static boolean mus_playing(void) { return false; }

static snddevice_t mus_devices[] = { SNDDEVICE_ADLIB, SNDDEVICE_SB, SNDDEVICE_GENMIDI };

music_module_t DG_music_module = {
    mus_devices,
    sizeof(mus_devices) / sizeof(mus_devices[0]),
    mus_init,
    mus_noop,          /* Shutdown */
    mus_setvol,
    mus_noop,          /* Pause  */
    mus_noop,          /* Resume */
    mus_register,
    mus_unregister,
    mus_play,
    mus_noop,          /* StopSong */
    mus_playing,
    NULL,              /* Poll: i_sound.c ya tolera NULL */
};

/* --------------------------------------------------------------- el arranque */

static esp_err_t codec_up(void)
{
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, NULL), TAG, "i2s_new_channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(OUT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_IO, .bclk = I2S_BCLK_IO, .ws = I2S_WS_IO,
            .dout = I2S_DOUT_IO, .din = I2S_GPIO_UNUSED,
        },
    };
    /* MCLK multiple: 256 (el default), NO el 384 del ejemplo. Review C2: la tabla
     * de coeficientes del ES8311 (es8311.c:85-88 del componente) NO tiene entrada
     * para 22050x384 = 8,4672 MHz -> esp_codec_dev_open devolveria NOT_SUPPORT y
     * el audio moriria en silencio. 22050x256 = 5,6448 MHz SI esta en la tabla.
     * (El ejemplo usaba 16000 Hz, cuyo x384 si existe — el patron no transfiere.)
     * Ademas esp_codec_dev_open reconfigura el reloj I2S desde fs (default 256). */
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s_std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s_enable");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = doom_i2c(),          /* bus compartido, ya creado por el display */
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "i2c ctrl_if");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .tx_handle = tx, .rx_handle = NULL };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "i2s data_if");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,   /* solo reproducir */
        .master_mode = false,
        .use_mclk = true,
        .pa_pin = -1,                                /* el PA lo manda el C5, no un GPIO */
        .pa_reverted = false,
        /* LOS VALORES REALES DEL KODE, no los del ejemplo: con pa_voltage=5.0 el
         * codec mete +3,5 dB digitales y recorta a partir de volumen ~93. */
        .hw_gain = { .pa_voltage = 4.2, .codec_dac_voltage = 3.3 },
        .mclk_div = 256,   /* tiene que casar con el MCLK real (22050x256, ver arriba) */
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311_codec_new");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev, ESP_FAIL, TAG, "esp_codec_dev_new");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = OUT_RATE,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_dev, &fs), TAG, "codec open");
    esp_codec_dev_set_out_vol(s_dev, VOL_START);
    return ESP_OK;
}

static esp_err_t pa_enable(void)
{
    /* Mismo patron que la SD (IO26): INPUT_OUTPUT para que el readback valga.
     * En la placa 6c:20 el boot-tweak del C5 ya lo deja alto; esto lo asegura
     * en cualquier otra. Polaridad por confirmar en F0. */
    esp_hosted_cp_gpio_config_t io = {
        .pin_bit_mask = (1ULL << C5_PA_CTRL),
        .mode = H_CP_GPIO_MODE_INPUT_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(esp_hosted_cp_gpio_config(&io), TAG, "rpc gpio config");
    return esp_hosted_cp_gpio_set_level(C5_PA_CTRL, 1);
}

esp_err_t doom_sound_init(void)
{
    esp_err_t e = codec_up();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "codec no disponible (%s): DOOM ira SIN sonido", esp_err_to_name(e));
        return e;
    }
    if ((e = pa_enable()) != ESP_OK) {
        ESP_LOGW(TAG, "PA_CTRL por RPC fallo (%s): quiza suene igual (boot-tweak del C5)",
                 esp_err_to_name(e));
    }
    /* Core 1, por encima del input (3): al bloquear en el DMA no acapara nada. */
    BaseType_t ok = xTaskCreatePinnedToCore(mixer_task, "doom_mix", 4096, NULL, 6, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "sin memoria para el mezclador");

    s_ready = true;
    ESP_LOGI(TAG, "SFX listos: %d canales, %d Hz, vol %d", NUM_CH, OUT_RATE, VOL_START);
    return ESP_OK;
}
