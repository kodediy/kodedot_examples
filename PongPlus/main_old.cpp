/*
 * Base Project for Kode Dot (ESP32-S3)
 * ------------------------------------------------------------
 * Purpose
 *  - Provide a clean, production-ready starting point that wires up
 *    all the on-board peripherals with clear, English documentation.
 *  - Keep implementations in this file for approachability; advanced
 *    board abstractions live under `lib/kodedot_bsp/`.
 *
 * Features covered
 *  - Display + LVGL v9 UI
 *  - Touch + IO Expander buttons
 *  - Addressable LED (WS2812B) with robust RMT init
 *  - SD card over SD_MMC (1-bit)
 *  - IMU (LSM6DSOX) + Magnetometer (LIS2MDL)
 *  - RTC (MAX31329)
 *  - PMIC / Charger (BQ25896)
 *  - Fuel Gauge (BQ27220)
 *
 * Guidance
 *  - All sections are documented with why/what they do and how to change
 *    common parameters.
 *  - Prefer short, frequent updates (1–60s) and avoid long blocking calls.
 *  - Serial prints are informative but minimal; adjust verbosity as needed.
 */
#include <Arduino.h>
#include <lvgl.h>
#include <kodedot/display_manager.h>
#include <TCA9555.h>
#include <kodedot/pin_config.h>
#include <Adafruit_NeoPixel.h>
// SD card (SD_MMC, 1-bit)
#include <FS.h>
#include <SD_MMC.h>
// IMU + Magnetometer
#include <Wire.h>
#include <Adafruit_LIS2MDL.h>
#include <Adafruit_LSM6DSOX.h>
#include <esp_pm.h>
// RTC MAX31329
#include <kode_MAX31329.h>
// PMIC BQ25896
#include <PMIC_BQ25896.h>
// Fuel gauge BQ27220
#include <BQ27220.h>
// Brand logo image (generated C array)
extern const lv_image_dsc_t logotipo;

// ---- Brand fonts (Inter) ----
// These symbols are provided by the generated font C files under src/fonts/
extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30;
extern const lv_font_t Inter_40;
extern const lv_font_t Inter_50;

// Display manager instance
DisplayManager display;

// UI labels
static lv_obj_t *touch_label;
static lv_obj_t *button_label;
static lv_obj_t *sd_label;
static lv_obj_t *imu_label;
static lv_obj_t *mag_label;
static lv_obj_t *rtc_label;
static lv_obj_t *pmic_label;
static lv_obj_t *gauge_label;

// IO Expander
static TCA9555 ioexp(IOEXP_I2C_ADDR);

// NeoPixel
static Adafruit_NeoPixel pixel(NEO_PIXEL_COUNT, NEO_PIXEL_PIN, LED_STRIP_COLOR_ORDER + LED_STRIP_TIMING);
static esp_pm_lock_handle_t g_no_light_sleep_lock = nullptr;

// -----------------------------------------------------------------------------
// Update cadences (ms) – tweak to balance responsiveness vs. CPU usage
// -----------------------------------------------------------------------------
static const uint32_t GUI_LOOP_DELAY_MS   = 5;      // loop() delay – target ~200 Hz
static const uint32_t IMU_UPDATE_MS       = 1000;   // IMU + MAG
static const uint32_t RTC_UPDATE_MS       = 1000;   // RTC (1 s)
static const uint32_t PMIC_UPDATE_MS      = 1000;   // Charger/PMIC status
static const uint32_t GAUGE_UPDATE_MS     = 1000;   // Fuel gauge

// -----------------------------------------------------------------------------
// Brand palette (see design guide)
// -----------------------------------------------------------------------------
static const lv_color_t KODE_BG_DARK        = lv_color_hex(0x000000); // background (brand: black)
static const lv_color_t KODE_TEXT_LIGHT     = lv_color_hex(0xFFFAF5); // normal light text
static const lv_color_t KODE_TEXT_MUTED     = lv_color_hex(0x9A948F); // titles / not highlighted
static const lv_color_t KODE_ACCENT         = lv_color_hex(0xFF7F1F); // accent
static const lv_color_t KODE_ACCENT_SECOND  = lv_color_hex(0x7B00FF); // secondary accent

// Global styles
static lv_style_t style_screen_bg;
static lv_style_t style_title;
static lv_style_t style_text;
static lv_style_t style_hint;
static lv_style_t style_accent;

static void setupBrandStyles() {
    lv_style_init(&style_screen_bg);
    lv_style_set_bg_color(&style_screen_bg, KODE_BG_DARK);

    lv_style_init(&style_title);
    lv_style_set_text_color(&style_title, KODE_TEXT_LIGHT);
    lv_style_set_text_font(&style_title, &Inter_50);

    lv_style_init(&style_text);
    lv_style_set_text_color(&style_text, KODE_TEXT_LIGHT);
    lv_style_set_text_font(&style_text, &Inter_20);

    lv_style_init(&style_hint);
    lv_style_set_text_color(&style_hint, KODE_TEXT_LIGHT);
    lv_style_set_text_font(&style_hint, &Inter_20);

    lv_style_init(&style_accent);
    lv_style_set_text_color(&style_accent, KODE_ACCENT);
    lv_style_set_text_font(&style_accent, &Inter_20);
}

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
void createUserInterface();
void createFontExamples(lv_obj_t *parent);
void updateTouchDisplay();
void updateButtonDisplay();
void initSdCardDemo();
static const char* sdTypeToText(uint8_t t);
void initImuAndMag();
void updateImuAndMag();
void initRtc();
void updateRtc();
void initPmic();
void updatePmic();
void initGauge();
void updateGauge();

void setup() {
    // ---- Boot diagnostics ----
    Serial.begin(115200);
    Serial.println("Starting Base Project with LVGL...");

    // Initialize display subsystem
    if (!display.init()) {
        Serial.println("Error: Failed to initialize display");
        while(1) {
            delay(1000);
        }
    }

    // Create the UI
    setupBrandStyles();
    createUserInterface();
    // Try to mount SD and show status
    initSdCardDemo();
    // Initialize IMU + Magnetometer
    initImuAndMag();
    // Initialize RTC
    initRtc();
    // Initialize PMIC/Charger
    initPmic();
    // Initialize Fuel Gauge
    initGauge();
    
    // Initialize IO Expander (inputs)
    if (!ioexp.begin(INPUT)) {
        Serial.println("Warning: IO Expander not connected");
    }

  // Configure TOP button (use internal pull-up for reliable reads)
  pinMode(BUTTON_TOP, INPUT_PULLUP);

    // Initialize NeoPixel
  // Keep ESP32-S3 out of light sleep while initializing RMT for NeoPixel.
  // RMT init can fail during light sleep transitions on S3 if not locked.
  if (!g_no_light_sleep_lock) {
      if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "neopixel", &g_no_light_sleep_lock) == ESP_OK) {
          esp_pm_lock_acquire(g_no_light_sleep_lock);
      }
  }
  pinMode(NEO_PIXEL_PIN, OUTPUT);
  pixel.begin();
  // Global brightness (~20%)
  pixel.setBrightness(51);
  pixel.clear();
  pixel.show();

  // Start LED off (idle)
  pixel.setPixelColor(0, pixel.Color(0, 0, 0));
  pixel.show();

    Serial.println("System ready!");
}

void loop() {
    // Pump display subsystem (LVGL timers + rendering)
    display.update();
    
    // Update touch coordinates on the UI
    updateTouchDisplay();
    updateButtonDisplay();
    updateImuAndMag();
    updateRtc();
    updatePmic();
    updateGauge();
    
    delay(GUI_LOOP_DELAY_MS);
}

/**
 * @brief Create the main user interface
 *
 * Minimal UI showing live status for SD, IMU/MAG, RTC, PMIC/Charger.
 * Extend this to add your own screens, themes, and widgets. For complex
 * apps, consider splitting screens into dedicated modules.
 */
void createUserInterface() {
    lv_obj_t * scr = lv_scr_act();
    // Apply brand background style and center content using flex layout
    lv_obj_add_style(scr, &style_screen_bg, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Compact padding/row spacing to ensure everything fits vertically
    lv_obj_set_style_pad_all(scr, 6, 0);
    lv_obj_set_style_pad_row(scr, 4, 0);
    // Top padding = 0; we'll space the logo from top using its own margin (20px)
    lv_obj_set_style_pad_top(scr, 0, 0);

    // Logo at the very top, tinted with brand light color (no textual title)
    lv_obj_t * img_logo = lv_image_create(scr);
    lv_image_set_src(img_logo, &logotipo);
    lv_obj_set_style_img_recolor_opa(img_logo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(img_logo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_margin_top(img_logo, 40, 0);

    // Content container: fills remaining space and centers its children
    lv_obj_t * content = lv_obj_create(scr);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Label for touch coordinates
    touch_label = lv_label_create(content);
    lv_obj_add_style(touch_label, &style_text, 0);
    lv_label_set_text(touch_label, "Touch: (-, -)");
    lv_obj_set_style_text_font(touch_label, &Inter_20, 0);

    // Label for buttons state (expander)
    button_label = lv_label_create(content);
    lv_obj_add_style(button_label, &style_text, 0);
    lv_label_set_text(button_label, "Button: none");
    lv_obj_set_style_text_font(button_label, &Inter_20, 0);

    // Ejemplos de diferentes fuentes
    // Omit font examples in base UI to save space

    // Label for SD status
    sd_label = lv_label_create(content);
    lv_obj_add_style(sd_label, &style_text, 0);
    lv_label_set_text(sd_label, "SD: --");
    lv_obj_set_style_text_font(sd_label, &Inter_20, 0);

    // Label for IMU + MAG
    imu_label = lv_label_create(content);
    lv_obj_add_style(imu_label, &style_text, 0);
    lv_label_set_text(imu_label, "IMU: --");
    lv_obj_set_style_text_font(imu_label, &Inter_20, 0);

    mag_label = lv_label_create(content);
    lv_obj_add_style(mag_label, &style_text, 0);
    lv_label_set_text(mag_label, "MAG: --");
    lv_obj_set_style_text_font(mag_label, &Inter_20, 0);

    // Label for RTC time
    rtc_label = lv_label_create(content);
    lv_obj_add_style(rtc_label, &style_text, 0);
    lv_label_set_text(rtc_label, "RTC: --");
    lv_obj_set_style_text_font(rtc_label, &Inter_20, 0);

    // Label for PMIC status
    pmic_label = lv_label_create(content);
    lv_obj_add_style(pmic_label, &style_text, 0);
    lv_label_set_text(pmic_label, "PMIC: --");
    lv_obj_set_style_text_font(pmic_label, &Inter_20, 0);

    // Optional hint
    lv_obj_t * hint = lv_label_create(content);
    lv_obj_add_style(hint, &style_hint, 0);
    lv_label_set_text(hint, "Hold TOP button to test LED colors");

    // Label for Fuel Gauge
    gauge_label = lv_label_create(content);
    lv_obj_set_style_text_font(gauge_label, &lv_font_montserrat_18, 0);
    lv_label_set_text(gauge_label, "BAT: --");
    lv_obj_set_style_text_color(gauge_label, KODE_TEXT_LIGHT, 0);
}

/**
 * @brief Create sample labels using different font sizes
 */
void createFontExamples(lv_obj_t *parent) {
    // Small font
    lv_obj_t * font_small = lv_label_create(parent);
    lv_obj_set_style_text_font(font_small, &lv_font_montserrat_14, 0);
    lv_label_set_text(font_small, "Montserrat 14px");
    lv_obj_set_style_text_color(font_small, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(font_small, LV_ALIGN_BOTTOM_LEFT, 20, -80);

    // Medium font
    lv_obj_t * font_medium = lv_label_create(parent);
    lv_obj_set_style_text_font(font_medium, &lv_font_montserrat_18, 0);
    lv_label_set_text(font_medium, "Montserrat 18px");
    lv_obj_set_style_text_color(font_medium, lv_color_hex(0xCCCCCC), 0);
    lv_obj_align(font_medium, LV_ALIGN_BOTTOM_LEFT, 20, -50);
    
    // Large font
    lv_obj_t * font_large = lv_label_create(parent);
    lv_obj_set_style_text_font(font_large, &lv_font_montserrat_42, 0);
    lv_label_set_text(font_large, "42");
    lv_obj_set_style_text_color(font_large, lv_color_hex(0x999999), 0);
    lv_obj_align(font_large, LV_ALIGN_BOTTOM_RIGHT, -30, -30);
}

/**
 * @brief Update the touch coordinates label
 */
void updateTouchDisplay() {
    if (!touch_label) return;
    
    int16_t x, y;
    if (display.getTouchCoordinates(x, y)) {
        lv_label_set_text_fmt(touch_label, "Touch: (%d, %d)", x, y);
    }
}

static inline bool isPressed(uint8_t pinIndex) {
    // Entradas con pull-up externa: activo en LOW
    int v = ioexp.read1(pinIndex);
    return (v != TCA9555_INVALID_READ) && (v == LOW);
}

static inline bool isGpioPressed(int gpio) {
  return digitalRead(gpio) == LOW; // activo en LOW por pull-up externa
}

void updateButtonDisplay() {
    if (!button_label) return;

    const char* status = "none";

  if (isGpioPressed(BUTTON_TOP)) {
      status = "BUTTON_TOP";
  } else if (isPressed(EXPANDER_BUTTON_BOTTOM)) {
        status = "BUTTON_BOTTOM";
    } else if (isPressed(EXPANDER_PAD_TOP)) {
        status = "PAD_TOP";
    } else if (isPressed(EXPANDER_PAD_BOTTOM)) {
        status = "PAD_BOTTOM";
    } else if (isPressed(EXPANDER_PAD_LEFT)) {
        status = "PAD_LEFT";
    } else if (isPressed(EXPANDER_PAD_RIGHT)) {
        status = "PAD_RIGHT";
    }

    // Always update label and LED color based on current state
    lv_label_set_text_fmt(button_label, "Button: %s", status);

    // Hold-to-test: if TOP is held > 700ms, cycle RGB quickly to verify LED
    static bool topWasPressed = false;
    static uint32_t topPressStart = 0;
    static uint8_t testPhase = 0;

    bool topNow = (strcmp(status, "BUTTON_TOP") == 0);
    uint32_t now = millis();
    if (topNow && !topWasPressed) {
        topWasPressed = true;
        topPressStart = now;
        testPhase = 0;
    } else if (!topNow && topWasPressed) {
        topWasPressed = false;
    }

    if (topWasPressed && (now - topPressStart > 700)) {
        // non-blocking cycle every 150ms
        static uint32_t lastStep = 0;
        if (now - lastStep > 150) {
            lastStep = now;
            testPhase = (testPhase + 1) % 3;
            uint32_t c = (testPhase == 0) ? pixel.Color(255,0,0)
                         : (testPhase == 1) ? pixel.Color(0,255,0)
                                            : pixel.Color(0,0,255);
            pixel.setPixelColor(0, c);
            pixel.show();
        }
        return;
    }

    // Normal mapping: only TOP lights the LED; all others keep it off
    uint32_t color = pixel.Color(0, 0, 0); // idle off
    if (strcmp(status, "BUTTON_TOP") == 0) {
        color = pixel.Color(255, 0, 0);      // red
    }
    pixel.setPixelColor(0, color);
    pixel.show();
}

// Boot LED test removed per request

static const char* sdTypeToText(uint8_t t) {
    switch (t) {
        case CARD_MMC: return "MMC";
        case CARD_SD: return "SDSC";
        case CARD_SDHC: return "SDHC";
        default: return "UNKNOWN";
    }
}

void initSdCardDemo() {
    Serial.println("Mounting SD_MMC (1-bit)...");

    // Assign custom pins from pin_config.h
    if (!SD_MMC.setPins(SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0)) {
        Serial.println("[SD] setPins failed");
        if (sd_label) {
            lv_label_set_text(sd_label, "SD: setPins failed");
            lv_obj_set_style_text_color(sd_label, KODE_TEXT_LIGHT, 0);
        }
        return;
    }

    // busWidth = 1 for 1-bit mode
    if (!SD_MMC.begin(SD_MOUNT_POINT, 1)) {
        Serial.println("[SD] Card mount failed");
        if (sd_label) {
            lv_label_set_text(sd_label, "SD: mount failed");
            lv_obj_set_style_text_color(sd_label, KODE_TEXT_LIGHT, 0);
        }
        return;
    }

    uint8_t type = SD_MMC.cardType();
    if (type == CARD_NONE) {
        Serial.println("[SD] No card attached");
        if (sd_label) {
            lv_label_set_text(sd_label, "SD: no card");
            lv_obj_set_style_text_color(sd_label, KODE_TEXT_LIGHT, 0);
        }
        return;
    }

    uint64_t sizeMB = SD_MMC.cardSize() / (1024ULL * 1024ULL);
    Serial.printf("[SD] Type=%s Size=%lluMB\n", sdTypeToText(type), sizeMB);

    if (sd_label) {
        char buf[64];
        snprintf(buf, sizeof(buf), "SD: OK (%s %lluMB)", sdTypeToText(type), sizeMB);
        lv_label_set_text(sd_label, buf);
        lv_obj_set_style_text_color(sd_label, KODE_TEXT_LIGHT, 0);
    }

    // Minimal demo: list root directory
    Serial.println("[SD] Listing root /");
    File root = SD_MMC.open("/");
    if (root && root.isDirectory()) {
        File f = root.openNextFile();
        while (f) {
            Serial.printf("  %s %s (%u)\n", f.isDirectory() ? "DIR " : "FILE", f.name(), (unsigned)f.size());
            f = root.openNextFile();
        }
    }
}

// ---- IMU + Magnetometer ----
// Reads basic motion + magnetic field for demos and quick health checks.
// Pins are configured in pin_config.h; wire is initialized once and reused.
static Adafruit_LSM6DSOX imu;
static Adafruit_LIS2MDL mag(12345);
static MAX31329 rtc;
static PMIC_BQ25896 bq;
static BQ27220 gauge;

void initImuAndMag() {
    // Inicializa bus I2C con pines de la placa
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);

    Serial.println("Init IMU (LSM6DSOX) + MAG (LIS2MDL)...");

    bool imu_ok = imu.begin_I2C();
    if (!imu_ok) {
        Serial.println("[IMU] LSM6DSOX not found");
        if (imu_label) {
            lv_label_set_text(imu_label, "IMU: not found");
            lv_obj_set_style_text_color(imu_label, KODE_TEXT_LIGHT, 0);
        }
    } else {
        Serial.println("[IMU] LSM6DSOX OK");
        if (imu_label) {
            lv_label_set_text(imu_label, "IMU: OK");
            lv_obj_set_style_text_color(imu_label, KODE_TEXT_LIGHT, 0);
        }
    }

    bool mag_ok = mag.begin();
    if (!mag_ok) {
        Serial.println("[MAG] LIS2MDL not found");
        if (mag_label) {
            lv_label_set_text(mag_label, "MAG: not found");
            lv_obj_set_style_text_color(mag_label, KODE_TEXT_LIGHT, 0);
        }
    } else {
        Serial.println("[MAG] LIS2MDL OK");
        if (mag_label) {
            lv_label_set_text(mag_label, "MAG: OK");
            lv_obj_set_style_text_color(mag_label, KODE_TEXT_LIGHT, 0);
        }
    }
}

// ---- RTC MAX31329 ----
static void rtcPrintToSerialAndUi() {
    if (!rtc.readTime()) {
        Serial.println("[RTC] readTime failed");
        if (rtc_label) {
            lv_label_set_text(rtc_label, "RTC: read fail");
            lv_obj_set_style_text_color(rtc_label, KODE_TEXT_LIGHT, 0);
        }
        return;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             rtc.t.year, rtc.t.month, rtc.t.day,
             rtc.t.hour, rtc.t.minute, rtc.t.second);
    Serial.printf("[RTC] %s\n", buf);
    if (rtc_label) {
        lv_label_set_text_fmt(rtc_label, "RTC: %s", buf);
        lv_obj_set_style_text_color(rtc_label, KODE_TEXT_LIGHT, 0);
    }
}

// ---- PMIC BQ25896 ----
// Shows power source (USB/Battery) and charge state (PRE/CARGA/COMPLETA).
// Note: To source 5V on the upper connector, enable OTG (boost) mode using
// setOTG_CONFIG(true). Keep disabled by default.
static void pmicUpdateUi(bool haveUsb, uint8_t chrg_stat) {
    if (!pmic_label) return;
    const char* fuente = haveUsb ? "USB" : "BATERIA";
    const char* estado = "IDLE";
    switch (chrg_stat) {
        case 1: estado = "PRE"; break;           // Pre-charge
        case 2: estado = "CARGA"; break;         // Fast charge (CC/CV)
        case 3: estado = "COMPLETA"; break;      // Done
        default: estado = "IDLE"; break;         // Not charging
    }
    lv_label_set_text_fmt(pmic_label, "Fuente: %s  Estado: %s",
                          fuente, estado);
    lv_obj_set_style_text_color(pmic_label, KODE_TEXT_LIGHT, 0);
}

void initPmic() {
    // Reutiliza Wire(48/47)
    bq.begin();
    delay(200);
    // Habilita conversión continua de ADC (1 Hz) para refrescar medidas
    bq.setCONV_RATE(true);

    // Primera lectura para UI
    auto vstat = bq.get_VBUS_STAT_reg();
    bool haveUsb = vstat.pg_stat;                    // Power Good on VBUS
    pmicUpdateUi(haveUsb, vstat.chrg_stat);

    // To source 5V on the upper connector, enable boost mode:
    // bq.setOTG_CONFIG(true);        // enable OTG/boost (5V out)
    // bq.setPFM_OTG_DIS(false);      // optional: allow/disallow PFM in OTG
}

void updatePmic() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < PMIC_UPDATE_MS) return;
    last = now;
    // En modo continuo no es necesario; si se desactiva, usar setCONV_START(true)
    auto vstat = bq.get_VBUS_STAT_reg();
    bool haveUsb = vstat.pg_stat;
    pmicUpdateUi(haveUsb, vstat.chrg_stat);
}

// ---- Fuel Gauge BQ27220 ----
void initGauge() {
    if (!gauge.begin(Wire, 0x55, -1, -1, 400000)) {
        Serial.println("[GAUGE] BQ27220 not found");
        if (gauge_label) {
            lv_label_set_text(gauge_label, "BAT: gauge not found");
            lv_obj_set_style_text_color(gauge_label, KODE_TEXT_LIGHT, 0);
        }
        return;
    }
    Serial.println("[GAUGE] BQ27220 OK");
}

void updateGauge() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 1000) return; // 1 s
    last = now;
    int soc = gauge.readStateOfChargePercent();
    int ma  = gauge.readCurrentMilliamps(); // positive = charging
    if (gauge_label) {
        lv_label_set_text_fmt(gauge_label, "BAT: %d%%  I=%dmA", soc, ma);
        lv_obj_set_style_text_color(gauge_label, KODE_TEXT_LIGHT, 0);
    }
}

void initRtc() {
    // Reutiliza Wire ya en pines 48/47
    rtc.begin();
    // Si necesitas fijar hora inicial, descomenta y ajusta:
    // rtc.t.year=2025; rtc.t.month=1; rtc.t.day=1; rtc.t.hour=0; rtc.t.minute=0; rtc.t.second=0; rtc.t.dayOfWeek=3;
    // rtc.writeTime();
    rtcPrintToSerialAndUi();
}

void updateRtc() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < RTC_UPDATE_MS) return;
    last = now;
    rtcPrintToSerialAndUi();
}

void updateImuAndMag() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < IMU_UPDATE_MS) return;
    last = now;

    // Read IMU
    sensors_event_t accel, gyro, temp;
    bool haveImu = imu.getEvent(&accel, &gyro, &temp);
    static float yawDeg = 0.0f; // integrated yaw from gyro (deg)
    static uint32_t lastYawMs = 0;

    float rollRad = 0.0f, pitchRad = 0.0f;
    if (haveImu) {
        const float ax = accel.acceleration.x;
        const float ay = accel.acceleration.y;
        const float az = accel.acceleration.z;

        // Roll and Pitch from accelerometer (in radians)
        rollRad  = atan2f(ay, az);
        pitchRad = atan2f(-ax, sqrtf(ay * ay + az * az));

        const float rollDeg  = rollRad * RAD_TO_DEG;
        const float pitchDeg = pitchRad * RAD_TO_DEG;

        // Gyro in deg/s for serial debug
        const float gx_dps = gyro.gyro.x * RAD_TO_DEG;
        const float gy_dps = gyro.gyro.y * RAD_TO_DEG;
        const float gz_dps = gyro.gyro.z * RAD_TO_DEG;

        // Integrate yaw from gyro Z (deg)
        if (lastYawMs == 0) lastYawMs = now;
        float dtSec = (now - lastYawMs) / 1000.0f;
        lastYawMs = now;
        yawDeg += gz_dps * dtSec;
        // Normalize to [0,360)
        while (yawDeg < 0.0f) yawDeg += 360.0f;
        while (yawDeg >= 360.0f) yawDeg -= 360.0f;

        Serial.printf("IMU  : roll=%.1f pitch=%.1f yaw=%.1f  | gyro(dps)=(%.1f, %.1f, %.1f)  temp=%.1f C\n",
                      rollDeg, pitchDeg, yawDeg, gx_dps, gy_dps, gz_dps, temp.temperature);
        if (imu_label) {
            char buf[96];
            snprintf(buf, sizeof(buf), "IMU roll=%.1f pitch=%.1f yaw=%.1f", rollDeg, pitchDeg, yawDeg);
            lv_label_set_text(imu_label, buf);
        }
    }

    // Read Magnetometer and show raw values
    sensors_event_t mev;
    if (mag.getEvent(&mev)) {
        const float mx = mev.magnetic.x;
        const float my = mev.magnetic.y;
        const float mz = mev.magnetic.z;
        Serial.printf("MAG  : raw(uT)=(%.1f, %.1f, %.1f)\n", mx, my, mz);
        if (mag_label) {
            char buf[96];
            snprintf(buf, sizeof(buf), "MAG x=%.1f y=%.1f z=%.1f uT", mx, my, mz);
            lv_label_set_text(mag_label, buf);
        }
    }
}