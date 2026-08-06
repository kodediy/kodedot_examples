/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Bring-up del panel: copia fiel de kode_board.c (el codigo que YA enciende esta
 * pantalla en placa), sin LVGL y con num_fbs=2. Cada desviacion esta comentada.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_io_expander_tca9554.h"

#include "doom_display.h"

static const char *TAG = "doom_display";

/* Timings GVO 2.13 MIPI (kode_board.c): refresh = 13e6/((410+14)*(502+14)) ~ 59,4 Hz */
#define DPI_CLK_MHZ   13
#define LCD_HSYNC     2
#define LCD_HBP       5
#define LCD_HFP       7
#define LCD_VSYNC     2
#define LCD_VBP       5
#define LCD_VFP       7
#define DSI_LANES     1
#define DSI_MBPS      480
#define PHY_LDO_CHAN  3
#define PHY_LDO_MV    2500
#define PIN_LCD_RST   7                        /* DSP_&_TP_RST (display+touch, activo-bajo) */

#define KODE_I2C_SDA  4
#define KODE_I2C_SCL  5
#define TCA6408_ADDR  ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000   /* 0x20, register-compatible */
#define EXP_PIN_DSP_PWR_EN  IO_EXPANDER_PIN_NUM_7   /* VCI_EN del panel, activo-ALTO */
#define EXP_PIN_DSI_TP_INT  IO_EXPANDER_PIN_NUM_6   /* irq tactil (entrada, no se usa aqui) */

/* Secuencia del proveedor (GVO 2.13 MIPI) — verbatim de kode_board.c. Ojo el
 * offset de columna 22 del CASET. */
static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t []){0x00}, 1, 0},   // page 0
    {0x3A, (uint8_t []){0x55}, 1, 0},   // COLMOD: RGB565
    {0x35, (uint8_t []){0x00}, 1, 0},   // TE on
    {0x53, (uint8_t []){0x20}, 1, 0},   // brightness control on
    {0x51, (uint8_t []){0xFF}, 1, 0},   // brightness = max
    {0x63, (uint8_t []){0xFF}, 1, 0},   // HBM brightness
    {0x2A, (uint8_t []){0x00, 0x16, 0x01, 0xAF}, 4, 0},  // CASET 22..431
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xF5}, 4, 0},  // RASET 0..501
    {0x11, NULL, 0, 60},    // sleep out + 60 ms
    {0x29, NULL, 0, 0},     // display on
};

static i2c_master_bus_handle_t  s_i2c;
static esp_io_expander_handle_t s_exp;
static esp_lcd_panel_handle_t   s_panel;
static uint16_t                *s_stage;
static uint16_t                *s_fb[2];      /* los 2 FB del driver (PSRAM) */
static int                      s_back = 1;   /* el driver arranca escaneando fb[0] */

i2c_master_bus_handle_t doom_i2c(void)      { return s_i2c; }
esp_io_expander_handle_t doom_expander(void){ return s_exp; }
uint16_t *doom_display_stage(void)          { return s_stage; }

/* I2C -> TCA6408 (5 reintentos: NACK pasajero visto en placa) -> P7=1 (VCI_EN). */
static void panel_power_on(void)
{
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = KODE_I2C_SDA,
        .scl_io_num = KODE_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &s_i2c));

    esp_err_t e = ESP_FAIL;
    for (int try = 1; try <= 5; try++) {
        e = esp_io_expander_new_i2c_tca9554(s_i2c, TCA6408_ADDR, &s_exp);
        if (e == ESP_OK) {
            if (try > 1) ESP_LOGW(TAG, "el TCA6408 contesto al intento %d (NACK pasajero)", try);
            break;
        }
        ESP_LOGW(TAG, "el TCA6408 no contesta (%s), intento %d/5", esp_err_to_name(e), try);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_ERROR_CHECK(e);

    ESP_ERROR_CHECK(esp_io_expander_set_dir(s_exp, EXP_PIN_DSP_PWR_EN, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_exp, EXP_PIN_DSP_PWR_EN, 1));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(s_exp, EXP_PIN_DSI_TP_INT, IO_EXPANDER_INPUT));
    ESP_LOGI(TAG, "panel alimentado (DSP_PWR_EN/VCI_EN = 1 via TCA6408 P7)");
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t doom_display_init(void)
{
    /* PHY del DSI: LDO interno canal 3 a 2,5 V (jamas 3300). */
    esp_ldo_channel_handle_t phy_ldo = NULL;
    esp_ldo_channel_config_t phy_cfg = { .chan_id = PHY_LDO_CHAN, .voltage_mv = PHY_LDO_MV };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&phy_cfg, &phy_ldo));

    /* Orden forzado por hardware: el VCI_EN cuelga del expansor I2C. */
    panel_power_on();

    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANES,
        .lane_bit_rate_mbps = DSI_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));

    esp_lcd_panel_io_handle_t dbi_io;
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io));

    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DPI_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565,   /* 24bpp exige h*v%8==0; 410*502 no cumple */
        /* LA diferencia con kode_board: DOBLE framebuffer. Con 1, draw_bitmap
         * escribe en el buffer que el DPI esta escaneando = tearing (la leccion
         * de los fogonazos azules, ui_home.c:107). Con 2 el driver copia al
         * trasero y conmuta el escaneo al terminar. */
        .num_fbs = 2,
        .video_timing = {
            .h_size = DOOM_LCD_W,
            .v_size = DOOM_LCD_H,
            .hsync_back_porch = LCD_HBP,
            .hsync_pulse_width = LCD_HSYNC,
            .hsync_front_porch = LCD_HFP,
            .vsync_back_porch = LCD_VBP,
            .vsync_pulse_width = LCD_VSYNC,
            .vsync_front_porch = LCD_VFP,
        },
    };

    co5300_vendor_config_t vendor_config = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(dbi_io, &lcd_dev_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* FBs a negro ANTES de encender: el DPI escanea desde el primer instante y la
     * memoria sin estrenar en RGB565 tira a azul (el fogonazo documentado).
     * Y msync C2M despues: el memset deja lineas sucias en cache que, escritas
     * de vuelta mas tarde, pisarian frames ya presentados (review I1). */
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2,
                                                       (void **)&s_fb[0], (void **)&s_fb[1]));
    memset(s_fb[0], 0, (size_t)DOOM_LCD_W * DOOM_LCD_H * 2);
    memset(s_fb[1], 0, (size_t)DOOM_LCD_W * DOOM_LCD_H * 2);
    esp_cache_msync(s_fb[0], (size_t)DOOM_LCD_W * DOOM_LCD_H * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_cache_msync(s_fb[1], (size_t)DOOM_LCD_W * DOOM_LCD_H * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    /* Staging propio en PSRAM, 64-alineado y de tamano 64-multiple (requisito del
     * PPA para buffer y buffer_size de salida). */
    s_stage = heap_caps_aligned_calloc(64, 1, DOOM_STAGE_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_stage, ESP_ERR_NO_MEM, TAG, "sin PSRAM para el staging");

    /* En este panel disp_on_off contesta "not supported" (kode_board.h): se registra
     * el resultado y a otra cosa; lo que manda es el brillo. */
    esp_err_t on_err = esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "disp_on_off(true): %s", esp_err_to_name(on_err));
    ESP_ERROR_CHECK(esp_lcd_panel_co5300_set_brightness(s_panel, 100));

    ESP_LOGI(TAG, "display listo: 410x502 RGB565, num_fbs=2, staging en PSRAM");
    return ESP_OK;
}

esp_err_t doom_display_present(void)
{
    /* FLIP DE VERDAD (review I1, verificado en esp_lcd_panel_dpi.c:600-649): con
     * un buffer EXTERNO, draw_bitmap copia al FB que se esta escaneando (= tearing).
     * Solo si el puntero ES uno de los FB del driver toma el camino sin copia:
     * writeback de cache + conmutar el escaneo a ese FB. Asi que: staging ->
     * memcpy al FB TRASERO -> draw_bitmap(ese FB) = flip del driver.
     *
     * Sin espera de refresh a proposito: DOOM entrega a 35 fps (28,6 ms) y el
     * conmutado tarda como mucho un barrido (16,8 ms) -> el siguiente memcpy
     * siempre llega con el flip hecho. F0 lo verifica visualmente. */
    memcpy(s_fb[s_back], s_stage, (size_t)DOOM_LCD_W * DOOM_LCD_H * 2);
    esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, DOOM_LCD_W, DOOM_LCD_H,
                                            s_fb[s_back]);
    if (e == ESP_OK) {
        s_back ^= 1;
    }
    return e;
}

esp_err_t doom_display_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    return esp_lcd_panel_co5300_set_brightness(s_panel, pct);
}
