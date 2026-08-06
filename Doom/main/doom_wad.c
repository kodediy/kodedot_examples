/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_hosted.h"
#include "esp_hosted_cp_gpio.h"

#include "doom_wad.h"

static const char *TAG = "doom_wad";

#define SD_MOUNT "/sd"
/* Pines del slot 0 (el de la microSD). El slot 1 es del enlace con el C5. */
#define SD_D0 39
#define SD_CLK 43
#define SD_CMD 44
#define SD_LDO_CHAN 4          /* net ESP_LDO_VO4 = dominio IO de la SDMMC */
#define C5_SD_PWR   26         /* C5 IO26 = SD_PWR, activo-bajo: 0 = encendida */
/* 1-bit a proposito: en 4-bit algunas tarjetas NO escriben, y en 1-bit ya vamos
 * al 97% del techo del bus, asi que 4-bit tampoco compraria gran cosa. */
#define SD_WIDTH    1
#define SD_FREQ_KHZ 40000

static SemaphoreHandle_t    s_cp_ready;
static sdmmc_card_t        *s_card;
static sd_pwr_ctrl_handle_t s_ldo;
static const char          *s_err = "sin arrancar";
static char                 s_wad_path[64];

const char *doom_wad_error(void) { return s_err; }

/* esp-hosted es el DUENO del controlador SDMMC (bug IDF #16233): sellos. */
static esp_err_t host_init_dummy(void) { return ESP_OK; }
static esp_err_t host_deinit_dummy(void) { return ESP_OK; }

static void hosted_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    /* ANTES de CP_INIT cualquier RPC se pierde en el vacio, sin error. */
    if (id == ESP_HOSTED_EVENT_CP_INIT) xSemaphoreGive(s_cp_ready);
}

static esp_err_t c5_link_up(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_cp_ready = xSemaphoreCreateBinary();
    if (!s_cp_ready) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, hosted_event_handler, NULL, NULL));

    if (esp_hosted_init() != ESP_OK) {
        s_err = "esp_hosted_init fallo";
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "connect_to_slave() — resetea el C5 por GPIO54");
    esp_hosted_connect_to_slave();

    if (xSemaphoreTake(s_cp_ready, pdMS_TO_TICKS(10000)) != pdTRUE) {
        s_err = "El C5 no contesta (sin CP_INIT en 10 s).\n"
                "O no tiene firmware, o el SDIO no monta.";
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "CP_INIT: enlace SDIO + RPC con el C5 OK");
    return ESP_OK;
}

static esp_err_t sd_power_on(void)
{
    /* INPUT_OUTPUT, NO output-only: con output el buffer de entrada del C5 esta
     * apagado y get_level() devuelve SIEMPRE 0 (falso positivo historico). */
    esp_hosted_cp_gpio_config_t io = {
        .pin_bit_mask = (1ULL << C5_SD_PWR),
        .mode = H_CP_GPIO_MODE_INPUT_OUTPUT,
    };
    esp_err_t e = esp_hosted_cp_gpio_config(&io);
    if (e != ESP_OK) { ESP_LOGW(TAG, "GPIO-expander del C5 no disponible: %s", esp_err_to_name(e)); return e; }
    if ((e = esp_hosted_cp_gpio_set_level(C5_SD_PWR, 0)) != ESP_OK) return e;
    int rb = -1;
    if (esp_hosted_cp_gpio_get_level(C5_SD_PWR, &rb) == ESP_OK && rb != 0)
        ESP_LOGW(TAG, "IO26 no se queda a 0 (leo %d)", rb);
    return ESP_OK;
}

static esp_err_t sd_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SD_FREQ_KHZ;
    host.init   = &host_init_dummy;
    host.deinit = &host_deinit_dummy;

    /* El canal del LDO se coge UNA vez y no se suelta ("already in use"). */
    if (!s_ldo) {
        sd_pwr_ctrl_ldo_config_t lc = { .ldo_chan_id = SD_LDO_CHAN };
        if (sd_pwr_ctrl_new_on_chip_ldo(&lc, &s_ldo) != ESP_OK) {
            s_err = "No se pudo abrir el LDO del dominio IO de la SD.";
            return ESP_FAIL;
        }
    }
    host.pwr_ctrl_handle = s_ldo;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = SD_WIDTH;
    slot.clk = SD_CLK; slot.cmd = SD_CMD; slot.d0 = SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,   /* jamas formatear solo */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mcfg, &s_card);
    if (ret != ESP_OK) {
        /* Soltar el slot A MANO (host.deinit es un sello) o el siguiente intento
         * muere con INVALID_STATE en cascada. Solo el 0: el 1 es del C5. */
        sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0);
        s_card = NULL;
        s_err = (ret == ESP_FAIL)
              ? "Hay tarjeta pero sin FAT. Formateala en un PC."
              : "No hay tarjeta, o no responde.";
        ESP_LOGE(TAG, "mount: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "montada en %s: %s, %llu MB, %d-bit @ %d kHz", SD_MOUNT,
             s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20,
             SD_WIDTH, s_card->max_freq_khz);
    return ESP_OK;
}

esp_err_t doom_wad_boot(void)
{
    esp_err_t e = c5_link_up();
    if (e != ESP_OK) return e;

    if (sd_power_on() != ESP_OK) {
        s_err = "El C5 no da corriente a la tarjeta (SD_PWR = C5 IO26).";
        return ESP_FAIL;
    }
    e = sd_mount();
    if (e != ESP_OK) return e;

    if (mkdir(DOOM_DIR, 0777) != 0 && errno != EEXIST)
        ESP_LOGW(TAG, "mkdir %s: errno %d", DOOM_DIR, errno);
    /* saves: el motor crea /sd/doom/.savegame/ el solo (chdir + configdir=".") */

    s_err = NULL;
    return ESP_OK;
}

esp_err_t doom_wad_seed_shareware(void)
{
    extern const uint8_t dw_start[] asm("_binary_doom1_wad_start");
    extern const uint8_t dw_end[]   asm("_binary_doom1_wad_end");
    size_t n = dw_end - dw_start;

    FILE *f = fopen(DOOM_DIR "/doom1.wad", "wb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "no puedo crear doom1.wad en la SD");
    size_t w = 0;
    while (w < n) {
        size_t chunk = (n - w > 65536) ? 65536 : (n - w);
        if (fwrite(dw_start + w, 1, chunk, f) != chunk) break;
        w += chunk;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    ESP_LOGI(TAG, "shareware sembrado: %u/%u bytes", (unsigned)w, (unsigned)n);
    return (w == n) ? ESP_OK : ESP_FAIL;
}

const char *doom_wad_load_embedded(void)
{
    extern const uint8_t dw_start[] asm("_binary_doom1_wad_start");
    extern const uint8_t dw_end[]   asm("_binary_doom1_wad_end");
    size_t n = (size_t)(dw_end - dw_start);

    /* Nombre PELADO a proposito: IdentifyIWADByName() compara contra su tabla
     * ("doom1.wad" = shareware). Con una ruta inventada no lo reconoceria. */
    static const char PATH[] = "doom1.wad";

    unsigned char *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        s_err = "Sin PSRAM para el WAD embebido.";
        return NULL;
    }
    /* La flash esta mapeada en memoria, asi que esto es un memcpy normal. Se copia
     * a PSRAM en vez de servir desde la flash para no competir con la icache
     * durante el juego (el motor pide punteros directos al WAD todo el rato). */
    memcpy(buf, dw_start, n);
    doom_wadfile_mem_register(PATH, buf, (unsigned int)n);

    ESP_LOGW(TAG, "SIN microSD: WAD embebido servido desde PSRAM (%u bytes). "
                  "Se juega igual; NO se guardan partidas ni el default.cfg.", (unsigned)n);
    s_err = NULL;
    return PATH;
}

const char *doom_wad_find(void)
{
    /* FATFS es case-insensitive: "doom1.wad" encuentra DOOM1.WAD. */
    static const char *CANDIDATES[] = {
        "doom2.wad", "doom.wad", "doom1.wad", "freedoom2.wad", "freedoom1.wad",
    };
    for (size_t i = 0; i < sizeof(CANDIDATES) / sizeof(CANDIDATES[0]); i++) {
        snprintf(s_wad_path, sizeof(s_wad_path), DOOM_DIR "/%s", CANDIDATES[i]);
        struct stat st;
        if (stat(s_wad_path, &st) == 0 && st.st_size > 0) {
            ESP_LOGI(TAG, "IWAD: %s (%ld bytes)", s_wad_path, (long)st.st_size);
            return s_wad_path;
        }
    }
    s_err = "No hay ningun IWAD en /sd/doom/.\n"
            "Copia doom1.wad (shareware), doom.wad,\n"
            "doom2.wad o freedoom*.wad a esa carpeta.";
    return NULL;
}

esp_err_t doom_wad_load(const char *path)
{
    struct stat st;
    ESP_RETURN_ON_FALSE(stat(path, &st) == 0, ESP_ERR_NOT_FOUND, TAG, "stat %s", path);

    if ((size_t)st.st_size > DOOM_WAD_RAM_MAX) {
        ESP_LOGW(TAG, "WAD de %ld bytes > %u: se queda en stdio (cargas de nivel mas lentas)",
                 (long)st.st_size, DOOM_WAD_RAM_MAX);
        return ESP_OK;
    }

    unsigned char *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "sin PSRAM para %ld bytes de WAD", (long)st.st_size);

    FILE *f = fopen(path, "rb");
    ESP_RETURN_ON_FALSE(f, ESP_ERR_NOT_FOUND, TAG, "fopen %s", path);

    int64_t t0 = esp_timer_get_time();
    size_t total = 0;
    while (total < (size_t)st.st_size) {
        size_t n = fread(buf + total, 1, 64 * 1024, f);
        if (n == 0) break;
        total += n;
    }
    fclose(f);
    if (total != (size_t)st.st_size) {
        heap_caps_free(buf);
        s_err = "La lectura del WAD se corto a mitad.";
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WAD en PSRAM: %u bytes en %lld ms (%.1f MB/s)",
             (unsigned)total, (esp_timer_get_time() - t0) / 1000,
             total / 1.048576 / (double)((esp_timer_get_time() - t0)));

    doom_wadfile_mem_register(path, buf, (unsigned int)total);
    return ESP_OK;
}
