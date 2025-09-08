/*
 * USmeter Demo for Kode Dot (ESP32-S3)
 * ------------------------------------------------------------
 * Demo: Mide distancia con un HC-SR04 (TRIG=GPIO42, ECHO=GPIO41)
 * y muestra el resultado en pantalla. Controla un láser en GPIO1,
 * que se activa mientras se pulsa el botón TOP. Usa botones del
 * expansor para añadir funcionalidades y el NeoPixel como feedback.
 */
#include <Arduino.h>
#include <lvgl.h>
#include <kodedot/display_manager.h>
#include <TCA9555.h>
#include <kodedot/pin_config.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <esp_pm.h>
#include <PMIC_BQ25896.h>

// ---- Brand fonts (Inter) ----
// These symbols are provided by the generated font C files under src/fonts/
extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30;
extern const lv_font_t Inter_40;
extern const lv_font_t Inter_50;
extern const lv_font_t Inter_130;
extern const lv_font_t Inter_150;

// Display manager instance
DisplayManager display;

// UI elements (clean meter)
static lv_obj_t *distance_value_label; // big number
static lv_obj_t *unit_label;           // cm/in
static lv_obj_t *laser_dot;            // small colored dot indicator
static lv_obj_t *history_ta;           // history box

// IO Expander
static TCA9555 ioexp(IOEXP_I2C_ADDR);

// BQ25896 Power Management IC
static PMIC_BQ25896 bq;

// NeoPixel
static Adafruit_NeoPixel pixel(NEO_PIXEL_COUNT, NEO_PIXEL_PIN, LED_STRIP_COLOR_ORDER + LED_STRIP_TIMING);
static esp_pm_lock_handle_t g_no_light_sleep_lock = nullptr;

// -----------------------------------------------------------------------------
// HC-SR04 and Laser pins
// -----------------------------------------------------------------------------
static const int HC_TRIG_PIN = 2;
static const int HC_ECHO_PIN = 1;
static const int LASER_PIN   = 42;   // Láser en GPIO42

// -----------------------------------------------------------------------------
// Update cadences (ms) – tweak to balance responsiveness vs. CPU usage
// -----------------------------------------------------------------------------
static const uint32_t GUI_LOOP_DELAY_MS   = 5;       // loop() delay ~200 Hz
static const uint32_t DIST_UPDATE_MS      = 300;     // medición HC-SR04 (más lenta)

// -----------------------------------------------------------------------------
// Brand palette (see design guide)
// -----------------------------------------------------------------------------
static const lv_color_t KODE_BG_DARK        = lv_color_hex(0x000000); // background (brand: black)
static const lv_color_t KODE_TEXT_LIGHT     = lv_color_hex(0xFFFAF5); // normal light text
static const lv_color_t KODE_TEXT_MUTED     = lv_color_hex(0x9A948F); // titles / not highlighted
static const lv_color_t KODE_ACCENT         = lv_color_hex(0xFF7F1F); // accent
static const lv_color_t KODE_ACCENT_SECOND  = lv_color_hex(0x7B00FF); // secondary accent
static const lv_color_t VALUE_GREEN         = lv_color_hex(0x90EE90); // light green for main value

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
void updateButtonsAndLaser();
void updateDistanceDemo();
void initBQ25896();
static float measureDistanceCm(uint32_t timeout_us = 25000);
static void setNeopixel(uint8_t r, uint8_t g, uint8_t b);
static uint8_t clampU8(int v) { return (v < 0) ? 0 : (v > 255 ? 255 : (uint8_t)v); }

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
    
    // I2C (for IO Expander and BQ25896)
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);

    // Initialize BQ25896 Power Management IC
    initBQ25896();

    // Initialize IO Expander (inputs)
    if (!ioexp.begin(INPUT)) {
        Serial.println("Warning: IO Expander not connected");
    }

    // Configure TOP button (use internal pull-up for reliable reads)
    pinMode(BUTTON_TOP, INPUT_PULLUP);

    // HC-SR04 pins
    pinMode(HC_TRIG_PIN, OUTPUT);
    pinMode(HC_ECHO_PIN, INPUT);
    digitalWrite(HC_TRIG_PIN, LOW);

    // Laser output
    pinMode(LASER_PIN, OUTPUT);
    digitalWrite(LASER_PIN, LOW);

    // Initialize NeoPixel
    // Keep ESP32-S3 out of light sleep while initializing RMT for NeoPixel.
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
    setNeopixel(0, 0, 0);

    Serial.println("System ready!");
}

void loop() {
    // Pump display subsystem (LVGL timers + rendering)
    display.update();
    
    // Update UI
    updateButtonsAndLaser();
    updateDistanceDemo();
    
    delay(GUI_LOOP_DELAY_MS);
}

/**
 * @brief Create the main user interface
 *
 * UI minimalista: número grande + unidades, indicador de láser y estado.
 */
void createUserInterface() {
    lv_obj_t * scr = lv_scr_act();
    // Apply brand background style and center content using flex layout
    lv_obj_add_style(scr, &style_screen_bg, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    // Remove padding on screen
    lv_obj_set_style_pad_all(scr, 0, 0);

    // Big number (lowered)
    distance_value_label = lv_label_create(scr);
    lv_obj_add_style(distance_value_label, &style_title, 0);
    lv_obj_set_style_text_font(distance_value_label, &Inter_150, 0);
    lv_obj_set_style_text_color(distance_value_label, VALUE_GREEN, 0);
    lv_label_set_text(distance_value_label, "—");
    lv_label_set_long_mode(distance_value_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(distance_value_label, LV_SIZE_CONTENT);
    lv_obj_align(distance_value_label, LV_ALIGN_TOP_MID, 0, 70);

    // Units (very top of the screen, y=0)
    unit_label = lv_label_create(scr);
    lv_obj_add_style(unit_label, &style_text, 0);
    lv_obj_set_style_text_font(unit_label, &Inter_50, 0);
    lv_label_set_text(unit_label, "cm");
    lv_obj_set_style_text_color(unit_label, KODE_TEXT_MUTED, 0);
    lv_label_set_long_mode(unit_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(unit_label, LV_SIZE_CONTENT);
    lv_obj_align(unit_label, LV_ALIGN_TOP_MID, 0, 0);

    // History text area (rounded corners, white border)
    history_ta = lv_textarea_create(scr);
    lv_obj_set_width(history_ta, LV_PCT(90));
    lv_obj_set_height(history_ta, 200);
    lv_textarea_set_one_line(history_ta, false);
    lv_textarea_set_password_mode(history_ta, false);
    lv_textarea_set_text(history_ta, "");
    lv_obj_set_style_bg_opa(history_ta, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(history_ta, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(history_ta, 2, 0);
    lv_obj_set_style_border_color(history_ta, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(history_ta, 20, 0);
    lv_obj_set_style_text_font(history_ta, &Inter_40, 0);
    lv_obj_set_style_text_color(history_ta, KODE_ACCENT, 0);
    lv_obj_align(history_ta, LV_ALIGN_CENTER, 0, 100);
    lv_textarea_set_align(history_ta, LV_TEXT_ALIGN_CENTER);

    // Laser dot (top-right corner of screen)
    laser_dot = lv_obj_create(scr);
    lv_obj_set_size(laser_dot, 18, 18);
    lv_obj_set_style_radius(laser_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(laser_dot, KODE_TEXT_MUTED, 0);
    lv_obj_set_style_border_width(laser_dot, 0, 0);
    lv_obj_remove_flag(laser_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(laser_dot, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(laser_dot, LV_ALIGN_TOP_RIGHT, -12, 12);

    // No bottom status line
}

static inline bool isPressed(uint8_t pinIndex) {
    // Entradas con pull-up externa: activo en LOW
    int v = ioexp.read1(pinIndex);
    return (v != TCA9555_INVALID_READ) && (v == LOW);
}

static inline bool isGpioPressed(int gpio) {
  return digitalRead(gpio) == LOW; // activo en LOW por pull-up externa
}

// -------------------- Demo State --------------------
static bool unitsInches = false;
static bool holdReading = false;
static uint8_t neopxBrightness = 51; // 0..255 (~20%)
static float lastValidCm = NAN; // para historial

static void setNeopixel(uint8_t r, uint8_t g, uint8_t b) {
    pixel.setPixelColor(0, pixel.Color(r, g, b));
    pixel.show();
}

static float measureDistanceCm(uint32_t timeout_us) {
    // Trigger 10us HIGH
    digitalWrite(HC_TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(HC_TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(HC_TRIG_PIN, LOW);

    // Measure echo HIGH duration
    unsigned long duration = pulseIn(HC_ECHO_PIN, HIGH, timeout_us);
    if (duration == 0) return NAN; // timeout -> no echo
    // Convert to cm: (duration_us * 0.0343) / 2
    float cm = (duration * 0.0343f) * 0.5f;
    return cm;
}

void updateButtonsAndLaser() {
    if (!laser_dot) return;

    // Read buttons
    bool top = isGpioPressed(BUTTON_TOP);
    bool padTop = isPressed(EXPANDER_PAD_TOP);
    bool padBottom = isPressed(EXPANDER_PAD_BOTTOM);
    bool padLeft = isPressed(EXPANDER_PAD_LEFT);
    bool padRight = isPressed(EXPANDER_PAD_RIGHT);
    bool btnBottom = isPressed(EXPANDER_BUTTON_BOTTOM);

    // Láser: activo mientras se pulsa TOP
    digitalWrite(LASER_PIN, top ? HIGH : LOW);
    // Update dot color (rojo cuando ON)
    lv_obj_set_style_bg_color(laser_dot, top ? lv_color_hex(0xFF0000) : KODE_TEXT_MUTED, 0);

    // Edge detection for toggles
    static bool prevPadTop=false, prevPadBottom=false, prevBtnBottom=false, prevPadLeft=false, prevPadRight=false;

    if (padTop && !prevPadTop) { // toggle units
        unitsInches = !unitsInches;
        // Refresh all displayed values and history
        // 1. Update main value and unit label
        if (unit_label) lv_label_set_text(unit_label, unitsInches ? "in" : "cm");
        // 2. Update current value display
        float toShow = lastValidCm;
        if (!isnan(toShow)) {
            char buf[24];
            float value = unitsInches ? (toShow / 2.54f) : toShow;
            if (unitsInches) snprintf(buf, sizeof(buf), "%.1f", value);
            else snprintf(buf, sizeof(buf), "%.0f", value);
            if (distance_value_label) lv_label_set_text(distance_value_label, buf);
        }
        // 3. Refresh history box: reformat all lines
        if (history_ta) {
            // Get all text, split by lines, reformat, and re-set
            char hist[1024];
            strncpy(hist, lv_textarea_get_text(history_ta), sizeof(hist)-1);
            hist[sizeof(hist)-1] = '\0';
            char *out = hist;
            char *in = hist;
            char *line;
            char newhist[1024] = "";
            while ((line = strsep(&in, "\n")) != NULL) {
                float val = atof(line);
                if (val == 0 && line[0] != '0') continue; // skip empty
                char newline[32];
                if (unitsInches) snprintf(newline, sizeof(newline), "%.1f in\n", val/2.54f);
                else snprintf(newline, sizeof(newline), "%.0f cm\n", val);
                strncat(newhist, newline, sizeof(newhist)-strlen(newhist)-1);
            }
            lv_textarea_set_text(history_ta, newhist);
        }
    }
    // PAD_BOTTOM sin uso ahora (antes: smoothing)
    if (btnBottom && !prevBtnBottom) { // toggle hold
        holdReading = !holdReading;
        if (holdReading && history_ta && !isnan(lastValidCm)) {
            char line[32];
            if (unitsInches) {
                float inches = lastValidCm / 2.54f;
                snprintf(line, sizeof(line), "%.1f in\n", inches);
            } else {
                snprintf(line, sizeof(line), "%.0f cm\n", lastValidCm);
            }
            lv_textarea_add_text(history_ta, line);
            lv_obj_scroll_to_y(history_ta, LV_COORD_MAX, LV_ANIM_OFF);
        }
    }
    if (padLeft && !prevPadLeft) {
        neopxBrightness = clampU8((int)neopxBrightness - 20);
        pixel.setBrightness(neopxBrightness);
        pixel.show();
    }
    if (padRight && !prevPadRight) {
        neopxBrightness = clampU8((int)neopxBrightness + 20);
        pixel.setBrightness(neopxBrightness);
        pixel.show();
    }

    prevPadTop = padTop;
    prevPadBottom = padBottom;
    prevBtnBottom = btnBottom;
    prevPadLeft = padLeft;
    prevPadRight = padRight;

    // No bottom status line anymore
}

// -----------------------------------------------------------------------------
// BQ25896 Power Management Initialization
// -----------------------------------------------------------------------------
void initBQ25896() {
    Serial.println("Initializing BQ25896 Power Management IC...");
    
    // Initialize BQ25896 - reutiliza Wire ya inicializado
    bq.begin();
    delay(200);
    
    // Habilita conversión continua de ADC (1 Hz) para refrescar medidas
    bq.setCONV_RATE(true);
    
    Serial.println("BQ25896 initialized successfully");
    
    // Habilitar OTG mode para proporcionar 5V en el conector superior
    // IMPORTANTE: Esto activa la salida de 5V
    bq.setOTG_CONFIG(true);        // enable OTG/boost (5V out)
    Serial.println("BQ25896 OTG 5V output enabled");
    
    // Opcional: configurar modo PFM en OTG (mejora eficiencia en cargas ligeras)
    bq.setPFM_OTG_DIS(false);      // allow PFM in OTG mode
    
    // Primera lectura para verificar estado
    auto vstat = bq.get_VBUS_STAT_reg();
    bool haveUsb = vstat.pg_stat;                    // Power Good on VBUS
    Serial.printf("BQ25896 USB connected: %s, Charge status: %d\n", 
                  haveUsb ? "YES" : "NO", vstat.chrg_stat);
    
    Serial.println("BQ25896 configuration complete - 5V OTG enabled");
}

void updateDistanceDemo() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < DIST_UPDATE_MS) return;
    last = now;

    static float lastDisplayed = NAN;
    static float filt = NAN; // simple holder

    if (!holdReading) {
        float cm = measureDistanceCm();
        if (!isnan(cm)) {
            filt = cm; // asignación directa, sin suavizado
        } else {
            // keep filt as-is on timeout
        }
    }

    float toShow = filt; // could be NAN
    if (!isnan(toShow)) {
        // Format big number without unit
        char buf[24];
        float value = unitsInches ? (toShow / 2.54f) : toShow;
    if (unitsInches) snprintf(buf, sizeof(buf), "%.1f", value);
    else snprintf(buf, sizeof(buf), "%.0f", value); // cm sin decimales
        if (distance_value_label && (isnan(lastDisplayed) || fabsf(lastDisplayed - toShow) > 0.05f)) {
            lv_label_set_text(distance_value_label, buf);
            lastDisplayed = toShow;
        }
    if (unit_label) lv_label_set_text(unit_label, unitsInches ? "in" : "cm");
        // NeoPixel feedback: verde si lectura válida; rojo si láser ON; azul si timeout
        if (digitalRead(LASER_PIN) == HIGH) setNeopixel(255, 0, 0);
        else setNeopixel(0, 64, 0);
    lastValidCm = toShow; // guarda última válida para historial
    } else {
        if (distance_value_label) lv_label_set_text(distance_value_label, "—");
        if (unit_label) lv_label_set_text(unit_label, unitsInches ? "in" : "cm");
        // timeout -> azul si láser OFF
        if (digitalRead(LASER_PIN) == LOW) setNeopixel(0, 0, 64);
    }
}