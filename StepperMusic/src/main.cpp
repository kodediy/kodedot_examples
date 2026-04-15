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
 *  - Demo "stepper tones": usa el driver como H-bridge de un motor paso a paso
 *    para generar sonidos (no mover el motor) con 4 botones de tonos.
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
#include <esp_pm.h>
#include <driver/gpio.h>
#include <esp_timer.h>
// Math for sine drive
#include <math.h>
// ESP-IDF LEDC driver for robust PWM
#include <driver/ledc.h>
// PMIC BQ25896 para OTG/5V
#include <Wire.h>
#include <PMIC_BQ25896.h>
// Brand logo image (generated C array)
extern const lv_image_dsc_t logotipo;
// Stepper background image
extern const lv_image_dsc_t Stepper;

// ---- Brand fonts (Inter) ----
// These symbols are provided by the generated font C files under src/fonts/
extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30;
extern const lv_font_t Inter_40;
extern const lv_font_t Inter_50;

// Display manager instance
DisplayManager display;

// App screens
static lv_obj_t *g_scrHome = nullptr; // Home screen (Stepper background + Play)
static lv_obj_t *g_scrPlay = nullptr; // Play screen (pads)

// Home screen builders
static void create_home_screen();
static void on_home_play(lv_event_t *e);

// UI elementos
static lv_obj_t *badge_fault;
static lv_obj_t *btn_tone1;
static lv_obj_t *btn_tone2;
static lv_obj_t *btn_tone3;
static lv_obj_t *btn_tone4;

// IO Expander
static TCA9555 ioexp(IOEXP_I2C_ADDR);

// NeoPixel
static Adafruit_NeoPixel pixel(NEO_PIXEL_COUNT, NEO_PIXEL_PIN, LED_STRIP_COLOR_ORDER + LED_STRIP_TIMING);
static esp_pm_lock_handle_t g_no_light_sleep_lock = nullptr;

// PMIC BQ25896 para OTG/5V
static PMIC_BQ25896 bq;

// -----------------------------------------------------------------------------
// Cadencias
// -----------------------------------------------------------------------------
static const uint32_t GUI_LOOP_DELAY_MS   = 5;       // ~200 Hz
static const uint32_t NEOPIXEL_BLINK_MS   = 500;     // parpadeo sutil

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
static lv_style_t style_badge_fault;
static lv_style_t style_pad_hi;

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
static void on_tone_button(lv_event_t * e);
static void updateNeoPixel();
static void pollFault();
static void initPmic();
static void updatePmic();
static void startToneHz(uint32_t hz);
static void stopTone();

// UI state (overlay) no longer used after split-screen Home/Play

// -----------------------------------------------------------------------------
// Pines DRV8411A (copiados del ejemplo funcionando)
// -----------------------------------------------------------------------------
constexpr int PIN_AIN1   = 42;
constexpr int PIN_AIN2   = 41;
constexpr int PIN_BIN1   = 39;
constexpr int PIN_BIN2   = 40;
constexpr int PIN_nFAULT = 3;

/* IPROPI -> ADC (A channel and B channel) */
constexpr int PIN_AIPROPI = 2;   /* ADC input for A-IPROPI */
constexpr int PIN_BIPROPI = 1;   /* ADC input for B-IPROPI */

/* IPROPI parameters (copiados del ejemplo funcionando) */
constexpr float VREF_V         = 3.3f;       /* VREF (V) */
constexpr float RIPROPI_OHMS   = 23700.0f;   /* Change to 10000.0f if you use 10 kΩ */
constexpr float AIPROPI_GAIN   = 200e-6f;    /* 200 µA/A (IPROPI gain) */

/* ADC configuration (copiado del ejemplo funcionando) */
constexpr int   ADC_BITS       = 12;         /* ADC resolution */
constexpr float ADC_VFULL      = 3.3f;       /* With 11 dB attenuation, ~3.3 V full scale */

/* PWM configuration base (usamos LEDC para tonos) */
constexpr uint32_t PWM_FREQ_HZ  = 20000;     /* 20 kHz (base) */
constexpr uint8_t  PWM_RES_BITS = 10;        /* 10-bit resolution */
constexpr uint32_t PWM_MAX_DUTY = (1u << PWM_RES_BITS) - 1u; /* 1023 */

static constexpr float EMA_ALPHA      = 0.2f;

// Estado de tono/stepper (no usamos movimiento real)
static bool g_tone_active = false;
static uint32_t g_tone_hz = 0;
static esp_timer_handle_t g_tone_timer = nullptr;
static volatile uint8_t g_tone_step = 0; // índice de subpaso (modo antiguo)
static uint8_t g_substeps_per_cycle = 2;  // 2: single coil FWD/REV, 4: dual coil FWD/COAST/REV/COAST (modo antiguo)

// Nuevo: excitación senoidal en cuadratura usando PWM por LEDC
static bool g_ledc_inited = false;
static constexpr uint32_t PWM_CARRIER_HZ = 30000; // 30 kHz fuera de banda audible
static constexpr ledc_mode_t LEDC_MODE_USED = LEDC_LOW_SPEED_MODE;
// Canales LEDC
static constexpr ledc_channel_t CH_A1 = LEDC_CHANNEL_0;
static constexpr ledc_channel_t CH_A2 = LEDC_CHANNEL_1;
static constexpr ledc_channel_t CH_B1 = LEDC_CHANNEL_2;
static constexpr ledc_channel_t CH_B2 = LEDC_CHANNEL_3;
// Sine engine
static esp_timer_handle_t g_sine_timer = nullptr;
static constexpr uint32_t AUDIO_SAMPLE_HZ = 8000; // 8 kHz de actualización de seno/coseno
static float g_phase = 0.0f;
static float g_phase_inc = 0.0f;
static float g_amp = 0.80f; // amplitud relativa (0..1), subir con cuidado para evitar FAULT

static inline void ledcSafeWrite(ledc_channel_t ch, uint32_t duty) {
    if (duty > PWM_MAX_DUTY) duty = PWM_MAX_DUTY;
    ledc_set_duty(LEDC_MODE_USED, ch, duty);
    ledc_update_duty(LEDC_MODE_USED, ch);
}

static void initPwmLedcIfNeeded() {
    if (g_ledc_inited) return;
    // Configurar temporizador LEDC
    ledc_timer_config_t tcfg = {};
    tcfg.speed_mode       = LEDC_MODE_USED;
    tcfg.duty_resolution  = (ledc_timer_bit_t)PWM_RES_BITS;
    tcfg.timer_num        = LEDC_TIMER_0;
    tcfg.freq_hz          = PWM_CARRIER_HZ;
    tcfg.clk_cfg          = LEDC_AUTO_CLK;
    ledc_timer_config(&tcfg);

    // Configurar canales
    auto cfg_ch = [&](ledc_channel_t ch, int gpio){
        ledc_channel_config_t c = {};
        c.gpio_num   = (gpio_num_t)gpio;
        c.speed_mode = LEDC_MODE_USED;
        c.channel    = ch;
        c.intr_type  = LEDC_INTR_DISABLE;
        c.timer_sel  = LEDC_TIMER_0;
        c.duty       = 0;
        c.hpoint     = 0;
        ledc_channel_config(&c);
    };
    cfg_ch(CH_A1, PIN_AIN1);
    cfg_ch(CH_A2, PIN_AIN2);
    cfg_ch(CH_B1, PIN_BIN1);
    cfg_ch(CH_B2, PIN_BIN2);
    ledcSafeWrite(CH_A1, 0);
    ledcSafeWrite(CH_A2, 0);
    ledcSafeWrite(CH_B1, 0);
    ledcSafeWrite(CH_B2, 0);
    g_ledc_inited = true;
}

enum class DriveMode : uint8_t { SingleCoil = 0, DualCoilCoast = 1, DualCoilBurst = 2, DualCoil66 = 3, StaggeredDual50 = 4, StaggeredDual66 = 5 };
static DriveMode g_drive_mode = DriveMode::DualCoilBurst; // modo más fuerte por defecto
static bool g_mode_forced = false;
static DriveMode g_forced_mode = DriveMode::DualCoilCoast;

// Función utilitaria para configurar PWM (declarada antes de usarla)
static inline void setPwmDuty(uint8_t pin, uint32_t duty) {
    if (duty > PWM_MAX_DUTY) duty = PWM_MAX_DUTY;
    ledcWrite(pin, duty);
}

// -----------------------------------------------------------------------------
// DRV8411A helpers con PWM progresivo
// -----------------------------------------------------------------------------
inline void motorA_coast()  { gpio_set_level((gpio_num_t)PIN_AIN1, 0); gpio_set_level((gpio_num_t)PIN_AIN2, 0); }
inline void motorB_coast()  { gpio_set_level((gpio_num_t)PIN_BIN1, 0); gpio_set_level((gpio_num_t)PIN_BIN2, 0); }

/* ADC counts -> volts (copiado del ejemplo funcionando) */
float adcVolts(int pin) {
  uint16_t raw = analogRead(pin);
  return (raw * ADC_VFULL) / ((1 << ADC_BITS) - 1);
}

/* IPROPI volts -> motor current (A) (copiado del ejemplo funcionando)
   Nota: mantenido por si quieres diagnosticar, no se usa para tonos. */
float ipropiToCurrentA(float v_ipropi) {
  return v_ipropi / (RIPROPI_OHMS * AIPROPI_GAIN);
}

// Función para poner el driver en estado seguro
static inline void safeStopMotors() {
    stopTone();
    motorA_coast();
    motorB_coast();
}

// Helpers de tono
static void tone_cb(void* /*arg*/) {
    uint8_t s = g_tone_step;
    if (g_drive_mode == DriveMode::SingleCoil) {
        // 2 subpasos: A FWD <-> A REV, B idle
        if ((s & 0x01) == 0) {
            gpio_set_level((gpio_num_t)PIN_AIN1, 1);
            gpio_set_level((gpio_num_t)PIN_AIN2, 0);
            gpio_set_level((gpio_num_t)PIN_BIN1, 0);
            gpio_set_level((gpio_num_t)PIN_BIN2, 0);
        } else {
            gpio_set_level((gpio_num_t)PIN_AIN1, 0);
            gpio_set_level((gpio_num_t)PIN_AIN2, 1);
            gpio_set_level((gpio_num_t)PIN_BIN1, 0);
            gpio_set_level((gpio_num_t)PIN_BIN2, 0);
        }
        g_tone_step = (s + 1) & 0x01;
        return;
    }

    if (g_drive_mode == DriveMode::DualCoilCoast) {
        // 4 subpasos FWD, COAST, REV, COAST (50% activo)
        switch (s & 0x03) {
            case 0: // FWD en A y B
                gpio_set_level((gpio_num_t)PIN_AIN1, 1);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 1);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 1: // COAST
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 2: // REV en A y B
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 1);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 1);
                break;
            default: // COAST
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
        }
        g_tone_step = (s + 1) & 0x03;
        return;
    }

    // DualCoilBurst: 8 subpasos (75% activo):
    // FWD,FWD,FWD,COAST, REV,REV,REV,COAST
    switch (s & 0x07) {
        case 0:
        case 1:
        case 2: // FWD triple
            gpio_set_level((gpio_num_t)PIN_AIN1, 1);
            gpio_set_level((gpio_num_t)PIN_AIN2, 0);
            gpio_set_level((gpio_num_t)PIN_BIN1, 1);
            gpio_set_level((gpio_num_t)PIN_BIN2, 0);
            break;
        case 3: // COAST
            gpio_set_level((gpio_num_t)PIN_AIN1, 0);
            gpio_set_level((gpio_num_t)PIN_AIN2, 0);
            gpio_set_level((gpio_num_t)PIN_BIN1, 0);
            gpio_set_level((gpio_num_t)PIN_BIN2, 0);
            break;
        case 4:
        case 5:
        case 6: // REV triple
            gpio_set_level((gpio_num_t)PIN_AIN1, 0);
            gpio_set_level((gpio_num_t)PIN_AIN2, 1);
            gpio_set_level((gpio_num_t)PIN_BIN1, 0);
            gpio_set_level((gpio_num_t)PIN_BIN2, 1);
            break;
        default: // 7: COAST
            gpio_set_level((gpio_num_t)PIN_AIN1, 0);
            gpio_set_level((gpio_num_t)PIN_AIN2, 0);
            gpio_set_level((gpio_num_t)PIN_BIN1, 0);
            gpio_set_level((gpio_num_t)PIN_BIN2, 0);
            break;
    }
    g_tone_step = (s + 1) & 0x07;
    return;

    // DualCoil66: 6 subpasos (66% activo): FWD,FWD,COAST,REV,REV,COAST
    if (g_drive_mode == DriveMode::DualCoil66) {
        switch (s % 6) {
            case 0:
            case 1: // FWD x2
                gpio_set_level((gpio_num_t)PIN_AIN1, 1);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 1);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 2: // COAST
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 3:
            case 4: // REV x2
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 1);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 1);
                break;
            default: // 5: COAST
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
        }
        g_tone_step = (s + 1) % 6;
        return;
    }

    // StaggeredDual50 (8 subpasos, 50% total, una bobina a la vez):
    // A FWD, COAST, A REV, COAST,  B FWD, COAST, B REV, COAST
    if (g_drive_mode == DriveMode::StaggeredDual50) {
        switch (s & 0x07) {
            case 0: // A FWD
                gpio_set_level((gpio_num_t)PIN_AIN1, 1);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 1: // COAST
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 2: // A REV
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 1);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 3: // COAST
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 4: // B FWD
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 1);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 5: // COAST
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 6: // B REV
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 1);
                break;
            default: // COAST
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
        }
        g_tone_step = (s + 1) & 0x07;
        return;
    }

    // StaggeredDual66 (12 subpasos, 66% total, una bobina a la vez):
    // A FWD, FWD, COAST, A REV, REV, COAST,  B FWD, FWD, COAST, B REV, REV, COAST
    if (g_drive_mode == DriveMode::StaggeredDual66) {
        uint8_t k = s % 12;
        // Reset all (coast) then set desired ones
        auto all_coast = [](){
            gpio_set_level((gpio_num_t)PIN_AIN1, 0);
            gpio_set_level((gpio_num_t)PIN_AIN2, 0);
            gpio_set_level((gpio_num_t)PIN_BIN1, 0);
            gpio_set_level((gpio_num_t)PIN_BIN2, 0);
        };
        all_coast();
        switch (k) {
            case 0:
            case 1: // A FWD x2
                gpio_set_level((gpio_num_t)PIN_AIN1, 1);
                gpio_set_level((gpio_num_t)PIN_AIN2, 0);
                break;
            case 2: // A COAST
                break;
            case 3:
            case 4: // A REV x2
                gpio_set_level((gpio_num_t)PIN_AIN1, 0);
                gpio_set_level((gpio_num_t)PIN_AIN2, 1);
                break;
            case 5: // A COAST
                break;
            case 6:
            case 7: // B FWD x2
                gpio_set_level((gpio_num_t)PIN_BIN1, 1);
                gpio_set_level((gpio_num_t)PIN_BIN2, 0);
                break;
            case 8: // B COAST
                break;
            case 9:
            case 10: // B REV x2
                gpio_set_level((gpio_num_t)PIN_BIN1, 0);
                gpio_set_level((gpio_num_t)PIN_BIN2, 1);
                break;
            default: // 11: B COAST
                break;
        }
        g_tone_step = (s + 1) % 12;
        return;
    }
}

static void startToneHz(uint32_t hz) {
    if (hz == 0) { stopTone(); return; }
    // Nuevo camino: PWM senoidal en cuadratura (más volumen y timbre limpio)
    initPwmLedcIfNeeded();

    // Configurar fase y frecuencia de audio
    g_phase = 0.0f;
    g_phase_inc = (2.0f * (float)M_PI) * ((float)hz / (float)AUDIO_SAMPLE_HZ);

    // Crear/arrancar timer de audio
    if (!g_sine_timer) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*){
            // Calcular seno y coseno actuales
            float s = sinf(g_phase);
            float c = cosf(g_phase);
            g_phase += g_phase_inc;
            if (g_phase > 2.0f * (float)M_PI) g_phase -= 2.0f * (float)M_PI;

            // Mapear -1..1 -> 0..1 para cada rama, con pequeña zona muerta para evitar shoot-through
            auto mapHalf = [](float x){
                // x en [-1,1]
                float xp = x * 0.5f + 0.5f; // [0,1]
                // Deadband 5% alrededor de 0 para mitigar corriente de cruzamiento
                if (xp > 0.5f - 0.025f && xp < 0.5f + 0.025f) xp = 0.5f;
                return xp;
            };

            float a_fwd = mapHalf(s) * g_amp;        // AIN1
            float a_rev = (1.0f - mapHalf(s)) * g_amp; // AIN2
            float b_fwd = mapHalf(c) * g_amp;        // BIN1
            float b_rev = (1.0f - mapHalf(c)) * g_amp; // BIN2

            uint32_t d_a1 = (uint32_t)(a_fwd * (float)PWM_MAX_DUTY);
            uint32_t d_a2 = (uint32_t)(a_rev * (float)PWM_MAX_DUTY);
            uint32_t d_b1 = (uint32_t)(b_fwd * (float)PWM_MAX_DUTY);
            uint32_t d_b2 = (uint32_t)(b_rev * (float)PWM_MAX_DUTY);

            ledcSafeWrite(CH_A1, d_a1);
            ledcSafeWrite(CH_A2, d_a2);
            ledcSafeWrite(CH_B1, d_b1);
            ledcSafeWrite(CH_B2, d_b2);
        };
        args.arg = nullptr;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "sine_audio";
        if (esp_timer_create(&args, &g_sine_timer) != ESP_OK) {
            Serial.println("Error creando esp_timer para audio senoidal");
            return;
        }
    } else {
        esp_timer_stop(g_sine_timer);
    }
    // Periodo de muestreo en microsegundos
    uint32_t sample_us = 1000000UL / AUDIO_SAMPLE_HZ;
    esp_timer_start_periodic(g_sine_timer, sample_us);

    g_tone_active = true;
    g_tone_hz = hz;
    Serial.printf("Tone ON (sine quadrature PWM): %u Hz\n", hz);
}

static void stopTone() {
    // Parar timers y poner salidas a 0 duty
    if (g_sine_timer) esp_timer_stop(g_sine_timer);
    if (g_tone_timer) esp_timer_stop(g_tone_timer);
    if (g_ledc_inited) {
        ledcSafeWrite(CH_A1, 0);
        ledcSafeWrite(CH_A2, 0);
        ledcSafeWrite(CH_B1, 0);
        ledcSafeWrite(CH_B2, 0);
    } else {
        gpio_set_level((gpio_num_t)PIN_AIN1, 0);
        gpio_set_level((gpio_num_t)PIN_AIN2, 0);
        gpio_set_level((gpio_num_t)PIN_BIN1, 0);
        gpio_set_level((gpio_num_t)PIN_BIN2, 0);
    }
    g_tone_active = false;
    g_tone_hz = 0;
    Serial.println("Tone OFF");
}

void setup() {
    // ---- Boot diagnostics ----
    Serial.begin(115200);
    Serial.println("Starting Motor Control Demo...");

    // Initialize display subsystem
    if (!display.init()) {
        Serial.println("Error: Failed to initialize display");
        while(1) {
            delay(1000);
        }
    }

    // Configuración de pines; si usamos LEDC se re-adjuntan a canales PWM
    pinMode(PIN_AIN1, OUTPUT);
    pinMode(PIN_AIN2, OUTPUT);
    pinMode(PIN_BIN1, OUTPUT);
    pinMode(PIN_BIN2, OUTPUT);
    pinMode(PIN_nFAULT, INPUT_PULLUP);

    // Estado seguro del H-bridge
    safeStopMotors();

    // Create the UI (two screens: Home and Play)
    setupBrandStyles();
    g_scrHome = lv_obj_create(NULL);
    g_scrPlay = lv_obj_create(NULL);
    create_home_screen();
    createUserInterface(); // builds Play screen on g_scrPlay
    lv_scr_load(g_scrHome);
    
    // IO Expander opcional para botones (si se usa en otra lógica)
    ioexp.begin(INPUT);
    
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
  // Brillo global 10%
  pixel.setBrightness(26);
  pixel.clear();
  pixel.show();

  // Start LED off (idle)
  pixel.setPixelColor(0, pixel.Color(0, 0, 0));
  pixel.show();

    // Inicializar LEDC PWM y dejar salidas en 0
    initPwmLedcIfNeeded();
    ledcSafeWrite(CH_A1, 0);
    ledcSafeWrite(CH_A2, 0);
    ledcSafeWrite(CH_B1, 0);
    ledcSafeWrite(CH_B2, 0);
    Serial.println("LEDC listo para tono senoidal");

    // ADC configuración como en el ejemplo funcionando
    Serial.println("Configurando ADC para IPROPI...");
    analogReadResolution(ADC_BITS);
    analogSetAttenuation(ADC_11db);

    Serial.printf("Configuración motores:\n");
    Serial.printf("  H-Bridge A: IN1=%d (tono), IN2=%d\n", PIN_AIN1, PIN_AIN2);
    Serial.printf("  H-Bridge B: IN1=%d, IN2=%d\n", PIN_BIN1, PIN_BIN2);
    Serial.printf("  nFAULT: pin %d\n", PIN_nFAULT);

    // Inicializar PMIC para OTG/5V
    initPmic();

    Serial.println("System ready!");
}

// -----------------------------------------------------------------------------
// PMIC BQ25896 para OTG/5V
// -----------------------------------------------------------------------------
static void initPmic() {
    Serial.println("Inicializando PMIC BQ25896 para OTG...");
    
    // Inicializar I2C con pines de la placa (reutilizar Wire ya configurado)
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    
    // Inicializar PMIC
    bq.begin();
    delay(200);
    
    // Habilitar conversión continua de ADC (1 Hz) para refrescar medidas
    bq.setCONV_RATE(true);
    
    // HABILITAR OTG/BOOST MODE - 5V de salida en el conector superior
    Serial.println("Habilitando modo OTG/Boost para 5V de salida...");
    bq.setOTG_CONFIG(true);        // enable OTG/boost (5V out)
    
    // Verificar estado inicial
    auto vstat = bq.get_VBUS_STAT_reg();
    bool haveUsb = vstat.pg_stat;  // Power Good on VBUS
    Serial.printf("PMIC: USB=%s, Estado=%d\n", haveUsb ? "SI" : "NO", vstat.chrg_stat);
    
    Serial.println("PMIC BQ25896 inicializado - OTG habilitado");
}

static void updatePmic() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 1000) return; // 1 segundo
    last = now;
    
    // Leer estado del PMIC
    auto vstat = bq.get_VBUS_STAT_reg();
    bool haveUsb = vstat.pg_stat;
    
    // Debug periódico
    static uint32_t debugCounter = 0;
    if (++debugCounter >= 10) { // Cada 10 segundos
        debugCounter = 0;
        Serial.printf("PMIC Status: USB=%s, Estado=%d\n", 
                     haveUsb ? "SI" : "NO", vstat.chrg_stat);
    }
}

void loop() {
    // Pump display subsystem (LVGL timers + rendering)
    display.update();
    pollFault();
    updateNeoPixel();
    updatePmic();  // Actualizar estado del PMIC
    
    delay(GUI_LOOP_DELAY_MS);
}

// Garantiza estado seguro si alguna vez se llamara a teardown
__attribute__((weak)) void endSketch() {
    safeStopMotors();
}

// Home screen implementation
static void on_home_play(lv_event_t *e) {
    (void)e;
    if (g_scrPlay) {
        lv_scr_load(g_scrPlay);
    }
}

static void create_home_screen() {
    if (!g_scrHome) g_scrHome = lv_obj_create(NULL);
    lv_obj_t *scr = g_scrHome;
    lv_obj_clean(scr);
    lv_obj_add_style(scr, &style_screen_bg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Background Stepper image full-screen
    lv_obj_t *img_bg = lv_image_create(scr);
    lv_image_set_src(img_bg, &Stepper);
    lv_obj_set_size(img_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_center(img_bg);
    lv_obj_clear_flag(img_bg, LV_OBJ_FLAG_CLICKABLE);

    // Overlay container for title and Play button
    lv_obj_t *overlay = lv_obj_create(scr);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    // Center children both vertically and horizontally
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(overlay, 16, 0);

    // Play button at bottom center
    lv_obj_t *btnPlay = lv_button_create(overlay);
    lv_obj_set_size(btnPlay, LV_PCT(60), 72);
    lv_obj_add_event_cb(btnPlay, on_home_play, LV_EVENT_CLICKED, nullptr);
    {
        lv_obj_t *l = lv_label_create(btnPlay);
        lv_obj_set_style_text_font(l, &Inter_40, 0);
        lv_label_set_text(l, "Play");
        lv_obj_center(l);
    }
    // Center the Play button in the screen
    lv_obj_center(btnPlay);
}

void createUserInterface() {
    lv_obj_t * scr = g_scrPlay ? g_scrPlay : lv_scr_act();
    // Apply brand background style
    lv_obj_add_style(scr, &style_screen_bg, 0);
    lv_obj_set_style_pad_all(scr, 10, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    // Prevent any scroll on the Play screen itself
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Plain dark background on Play screen (no Stepper image here)
    
    // Logo pequeño en la esquina superior
    lv_obj_t * img_logo = lv_image_create(scr);
    lv_image_set_src(img_logo, &logotipo);
    lv_obj_set_style_img_recolor_opa(img_logo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(img_logo, LV_OPA_TRANSP, 0);
    lv_obj_align(img_logo, LV_ALIGN_TOP_MID, 0, 5);

    // Playfield container (column) similar to Simon layout
    lv_obj_t *play = lv_obj_create(scr);
    lv_obj_set_size(play, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(play, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(play, 0, 0);
    lv_obj_set_flex_flow(play, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(play, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(play, 6, 0);
    lv_obj_set_style_pad_column(play, 0, 0);
    // Disable scrolling and move the whole pad area down by ~30px
    lv_obj_clear_flag(play, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(play, LV_ALIGN_CENTER, 0, 30);

    // Highlight style for pads (white outline)
    lv_style_init(&style_pad_hi);
    lv_style_set_outline_width(&style_pad_hi, 8);
    lv_style_set_outline_color(&style_pad_hi, lv_color_hex(0xFFFFFF));

    // Pad colors (R, G, B, Y)
    const lv_color_t padColors[4] = {
        lv_color_hex(0xFF3B30),
        lv_color_hex(0x34C759),
        lv_color_hex(0x007AFF),
        lv_color_hex(0xFFCC00)
    };

    auto makePadRow = [&](uint8_t rowIndex) {
        lv_obj_t *row = lv_obj_create(play);
        lv_obj_set_size(row, LV_PCT(92), 160);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 16, 0);
        // Prevent row from scrolling when swiping over buttons
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        for (uint8_t col = 0; col < 2; col++) {
            uint8_t id = rowIndex * 2 + col;
            lv_obj_t *b = lv_button_create(row);
            lv_obj_set_size(b, 140, 140);
            lv_obj_set_style_radius(b, 20, 0);
            lv_obj_set_style_border_width(b, 0, 0);
            lv_obj_set_style_bg_color(b, padColors[id], 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            // Ensure button itself cannot initiate scroll
            lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_event_cb(b, on_tone_button, LV_EVENT_PRESSED, nullptr);
            lv_obj_add_event_cb(b, on_tone_button, LV_EVENT_RELEASED, nullptr);

            // Label for note
            lv_obj_t *l = lv_label_create(b);
            lv_obj_set_style_text_font(l, &Inter_40, 0);
            const char *names[4] = {"Do","Re","Mi","Fa"};
            lv_label_set_text(l, names[id]);
            lv_obj_center(l);

            // Save references
            if (id == 0) btn_tone1 = b;
            else if (id == 1) btn_tone2 = b;
            else if (id == 2) btn_tone3 = b;
            else btn_tone4 = b;
        }
    };

    makePadRow(0);
    makePadRow(1);

    // Badge FAULT (relativo al playfield bottom area)
    lv_style_init(&style_badge_fault);
    lv_style_set_bg_color(&style_badge_fault, lv_color_hex(0xD73A49));
    lv_style_set_text_color(&style_badge_fault, lv_color_hex(0xFFFFFF));
    lv_style_set_radius(&style_badge_fault, 8);
    lv_style_set_pad_all(&style_badge_fault, 8);

    badge_fault = lv_label_create(scr);
    lv_obj_add_style(badge_fault, &style_badge_fault, 0);
    lv_label_set_text(badge_fault, "FAULT");
    lv_obj_align(badge_fault, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_add_flag(badge_fault, LV_OBJ_FLAG_HIDDEN);
}
// ---- Eventos/UI y lógica ----
static void on_tone_button(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * target = lv_event_get_target_obj(e);
    // Elegimos 4 tonos agradables (C5, D5, E5, G5)
    constexpr uint32_t TONE1 = 523; // Do (C5)
    constexpr uint32_t TONE2 = 587; // Re (D5)
    constexpr uint32_t TONE3 = 659; // Mi (E5)
    constexpr uint32_t TONE4 = 784; // Fa (G5)

    uint32_t hz = 0;
    if (target == btn_tone1) hz = TONE1;
    else if (target == btn_tone2) hz = TONE2;
    else if (target == btn_tone3) hz = TONE3;
    else if (target == btn_tone4) hz = TONE4;

    if (code == LV_EVENT_PRESSED) {
        // Highlight pad on press
        lv_obj_add_style(target, &style_pad_hi, 0);
        startToneHz(hz);
    } else if (code == LV_EVENT_RELEASED) {
        // Remove highlight on release
        lv_obj_remove_style(target, &style_pad_hi, 0);
        stopTone();
    }
}

// Overlay callback removed; Home screen controls navigation.

static void pollFault() {
    static bool faultLatched = false;
    static uint32_t lowSince = 0;
    bool low = (digitalRead(PIN_nFAULT) == LOW);
    uint32_t now = millis();
    if (low) {
        if (lowSince == 0) lowSince = now;
        // Debounce 3 ms
        if (!faultLatched && (now - lowSince) > 3) {
            safeStopMotors();
            if (badge_fault) lv_obj_clear_flag(badge_fault, LV_OBJ_FLAG_HIDDEN);
            faultLatched = true;
            Serial.println("FAULT detectado - motores parados");
        }
    } else {
        lowSince = 0;
        if (faultLatched) {
            if (badge_fault) lv_obj_add_flag(badge_fault, LV_OBJ_FLAG_HIDDEN);
            faultLatched = false;
            Serial.println("FAULT recuperado");
        }
    }
}

static void updateNeoPixel() {
    static uint32_t lastBlink = 0;
    static bool on = false;
    bool moving = g_tone_active;
    uint32_t now = millis();
    if (!moving) {
        if (on) {
            on = false;
            pixel.setPixelColor(0, pixel.Color(0,0,0));
            pixel.show();
        }
        return;
    }
    if (now - lastBlink >= NEOPIXEL_BLINK_MS) {
        lastBlink = now;
        on = !on;
        uint8_t d = on ? 20 : 0; // brillo sutil
        pixel.setPixelColor(0, pixel.Color(d, d, d));
        pixel.show();
    }
}