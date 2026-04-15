/*
 * Plant Monitor for Kode Dot (ESP32-S3)
 * ------------------------------------------------------------
 * Purpose
 *  - A demonstration plant monitoring system that shows animated plant status
 *  - Interactive moisture level control with D-pad buttons
 *  - Animated plant images that change based on moisture level
 *
 * Features
 *  - Plant animation: Small -> Mid -> Big when moisture >= 30%
 *  - Sad animation: Sad -> VerySad when moisture < 30%
 *  - Interactive moisture control with UP/DOWN D-pad buttons
 *  - Realistic moisture fluctuation with random variations
 *  - Clean UI with title and moisture percentage display
 */
#include <Arduino.h>
#include <lvgl.h>
#include <kodedot/display_manager.h>
#include <TCA9555.h>
#include <kodedot/pin_config.h>
#include <led_manager/LEDManager.h>
#include <esp32-hal-ledc.h>
// PMIC / OTG 5V
#include <Wire.h>
#include <PMIC_BQ25896.h>

// Plant images (generated C arrays)
extern const lv_image_dsc_t Small;
extern const lv_image_dsc_t Mid;
extern const lv_image_dsc_t Big;
extern const lv_image_dsc_t Sad;
extern const lv_image_dsc_t VerySad;

// ---- Brand fonts (Inter) ----
// These symbols are provided by the generated font C files under src/fonts/
extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30;
extern const lv_font_t Inter_40;
extern const lv_font_t Inter_50;

// Display manager instance
DisplayManager display;

// UI components
static lv_obj_t *title_label;
static lv_obj_t *moisture_label;
static lv_obj_t *moisture_box;
static lv_obj_t *plant_image;

// IO Expander for buttons
static TCA9555 ioexp(IOEXP_I2C_ADDR);

// NeoPixel
static LEDManager led;

// PMIC for 5V OTG
static PMIC_BQ25896 bq;

// Plant Monitor State
static float moisture_percentage = 75.0f;  // Start with healthy plant
static uint8_t animation_frame = 0;        // 0, 1, 2 for Small->Mid->Big or 0,1 for Sad->VerySad
static uint32_t last_animation_time = 0;
static uint32_t last_fluctuation_time = 0;
static bool use_real_soil = false;         // false = demo/simulated, true = real ADC from GPIO13
static const int SOIL_PIN = 13;            // Capacitive soil sensor analog pin
static float soil_filtered = -1.0f;        // EMA filter for real sensor

static inline int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int soilAnalogToPercent(int raw) {
    // Simple linear mapping with inversion (typical capacitive sensor: lower = wet, higher = dry)
    // Adjust these to your sensor; we keep conservative defaults
    const int ADC_WET = 1500; // raw when fully wet
    const int ADC_DRY = 3200; // raw when dry (in air)
    raw = clamp_int(raw, min(ADC_WET, ADC_DRY), max(ADC_WET, ADC_DRY));
    float pct = 100.0f * (float)(ADC_DRY - raw) / (float)(ADC_DRY - ADC_WET);
    if (pct < 0.0f) pct = 0.0f; if (pct > 100.0f) pct = 100.0f;
    return (int)(pct + 0.5f);
}

// -----------------------------------------------------------------------------
// Pump control via DRV8411A motor driver channels
// -----------------------------------------------------------------------------
static constexpr int PIN_AIN1 = 42; // A-IN1 (left driver)
static constexpr int PIN_AIN2 = 41; // A-IN2 (left driver)
static constexpr int PIN_BIN1 = 39; // B-IN1 (right driver)
static constexpr int PIN_BIN2 = 40; // B-IN2 (right driver)
static constexpr int PIN_nFAULT = 3; // nFAULT like example
static constexpr uint32_t PWM_FREQ_HZ  = 20000; // 20 kHz
static constexpr uint8_t  PWM_RES_BITS = 10;    // 10-bit
static constexpr uint32_t PWM_MAX_DUTY = (1u << PWM_RES_BITS) - 1u;

// Simple pump sequence: 5s forward @100% only
static bool pump_seq_active = false;
static uint32_t pump_seq_end_ms = 0;
static bool sim_ramp_active = false;
static float sim_ramp_from = 0.0f;
static float sim_ramp_to = 83.0f;
static uint32_t sim_ramp_start_ms = 0;
static const uint32_t sim_ramp_duration_ms = 10000; // 10 seconds
static const uint32_t sim_ramp_delay_ms = 2000; // 2 second delay before starting growth
static float sim_ramp_last_noise = 0.0f; // for smooth noise generation
// Select which channel to use for the pump by default (true=B, false=A)
static constexpr bool PUMP_USE_MOTOR_B = false;

// Match example: small wrapper that writes PWM by pin
static inline void setPwmDuty(uint8_t pin, uint32_t duty) {
    if (duty > PWM_MAX_DUTY) duty = PWM_MAX_DUTY;
    ledcWrite(pin, duty);
}

// DRV8411A channel helpers (exact style as example)
static inline void motorA_coast()  { setPwmDuty(PIN_AIN1, 0);            setPwmDuty(PIN_AIN2, 0);            }
static inline void motorA_brake()  { setPwmDuty(PIN_AIN1, PWM_MAX_DUTY); setPwmDuty(PIN_AIN2, PWM_MAX_DUTY); }
static inline void motorA_fwd(uint32_t duty) { setPwmDuty(PIN_AIN1, duty); setPwmDuty(PIN_AIN2, 0);            }
static inline void motorA_rev(uint32_t duty) { setPwmDuty(PIN_AIN1, 0);    setPwmDuty(PIN_AIN2, duty);         }

// DRV8411A B-channel helpers
static inline void motorB_coast()  { setPwmDuty(PIN_BIN1, 0);            setPwmDuty(PIN_BIN2, 0);            }
static inline void motorB_brake()  { setPwmDuty(PIN_BIN1, PWM_MAX_DUTY); setPwmDuty(PIN_BIN2, PWM_MAX_DUTY); }
static inline void motorB_fwd(uint32_t duty) { setPwmDuty(PIN_BIN1, duty); setPwmDuty(PIN_BIN2, 0);            }
static inline void motorB_rev(uint32_t duty) { setPwmDuty(PIN_BIN1, 0);    setPwmDuty(PIN_BIN2, duty);         }

static inline void pump_coast() {
    // Stop both PWM outputs
    motorA_coast();
    motorB_coast();
}

static inline void pump_forward_percent(int percent) {
    if (percent < 0) percent = 0; if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)((percent / 100.0f) * PWM_MAX_DUTY + 0.5f);
    
    // Check fault before starting
    if (digitalRead(PIN_nFAULT) == LOW) {
        Serial.println("ERROR: nFAULT is LOW - cannot start pump!");
        return;
    }
    
    if (PUMP_USE_MOTOR_B) {
        motorB_fwd(duty);
        Serial.printf("Pump FWD on BIN1=%d (duty=%u/%u = %.1f%%) [B]\n", 
                     PIN_BIN1, duty, PWM_MAX_DUTY, (duty*100.0f)/PWM_MAX_DUTY);
    } else {
        motorA_fwd(duty);
        Serial.printf("Pump FWD on AIN1=%d (duty=%u/%u = %.1f%%) [A]\n", 
                     PIN_AIN1, duty, PWM_MAX_DUTY, (duty*100.0f)/PWM_MAX_DUTY);
    }
}

static inline void pump_reverse_percent(int percent) {
    if (percent < 0) percent = 0; if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)((percent / 100.0f) * PWM_MAX_DUTY + 0.5f);
    
    // Check fault before starting
    if (digitalRead(PIN_nFAULT) == LOW) {
        Serial.println("ERROR: nFAULT is LOW - cannot start pump!");
        return;
    }
    
    if (PUMP_USE_MOTOR_B) {
        motorB_rev(duty);
        Serial.printf("Pump REV on BIN2=%d (duty=%u/%u = %.1f%%) [B]\n", 
                     PIN_BIN2, duty, PWM_MAX_DUTY, (duty*100.0f)/PWM_MAX_DUTY);
    } else {
        motorA_rev(duty);
        Serial.printf("Pump REV on AIN2=%d (duty=%u/%u = %.1f%%) [A]\n", 
                     PIN_AIN2, duty, PWM_MAX_DUTY, (duty*100.0f)/PWM_MAX_DUTY);
    }
}

static void initPmicForOtg() {
    Serial.println("Inicializando PMIC BQ25896 para OTG...");
    
    // Initialize I2C and enable 5V OTG to power pump + sensor
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    bq.begin();
    delay(200);
    
    // Enable continuous ADC conversion (1 Hz) to refresh measurements
    bq.setCONV_RATE(true);
    
    // ENABLE OTG/BOOST MODE - 5V output on top connector
    Serial.println("Habilitando modo OTG/Boost para 5V de salida...");
    bq.setOTG_CONFIG(true); // enable boost 5V
    
    // Verify initial status
    auto vstat = bq.get_VBUS_STAT_reg();
    bool haveUsb = vstat.pg_stat;  // Power Good on VBUS
    Serial.printf("PMIC: USB=%s, Estado=%d\n", haveUsb ? "SI" : "NO", vstat.chrg_stat);
    
    Serial.println("PMIC BQ25896 inicializado - OTG habilitado");
}

static void updatePmic() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 1000) return; // 1 second
    last = now;
    
    // Read PMIC status
    auto vstat = bq.get_VBUS_STAT_reg();
    bool haveUsb = vstat.pg_stat;
    
    // Periodic debug
    static uint32_t debugCounter = 0;
    if (++debugCounter >= 10) { // Every 10 seconds
        debugCounter = 0;
        Serial.printf("PMIC Status: USB=%s, Estado=%d\n", 
                     haveUsb ? "SI" : "NO", vstat.chrg_stat);
    }
}

// -----------------------------------------------------------------------------
// Update cadences (ms) – Plant Monitor timing
// -----------------------------------------------------------------------------
static const uint32_t GUI_LOOP_DELAY_MS     = 5;     // loop() delay – target ~200 Hz
static const uint32_t ANIMATION_UPDATE_MS   = 1000;  // Plant image animation (1 second)
static const uint32_t FLUCTUATION_UPDATE_MS = 500;   // Moisture fluctuation (0.5 seconds)
static const uint32_t BUTTON_CHECK_MS       = 50;    // Button responsiveness

// -----------------------------------------------------------------------------
// Brand palette (see design guide)
// -----------------------------------------------------------------------------
static const lv_color_t KODE_BG_DARK        = lv_color_hex(0x000000); // background (brand: black)
static const lv_color_t KODE_TEXT_LIGHT     = lv_color_hex(0xFFFAF5); // normal light text
static const lv_color_t KODE_TEXT_MUTED     = lv_color_hex(0x9A948F); // titles / not highlighted
static const lv_color_t KODE_ACCENT         = lv_color_hex(0xFF7F1F); // accent
static const lv_color_t KODE_ACCENT_SECOND  = lv_color_hex(0x7B00FF); // secondary accent
static const lv_color_t KODE_PLANT_GREEN    = lv_color_hex(0x228B22); // darker plant green (forest green)
static const lv_color_t KODE_NEAR_BLACK     = lv_color_hex(0x1A1A1A); // near black

// Global styles
static lv_style_t style_screen_bg;
static lv_style_t style_title;
static lv_style_t style_text;
static lv_style_t style_hint;
static lv_style_t style_accent;
static lv_style_t style_plant_title;
static lv_style_t style_irrigation;

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

    lv_style_init(&style_plant_title);
    lv_style_set_text_color(&style_plant_title, KODE_PLANT_GREEN);
    lv_style_set_text_font(&style_plant_title, &Inter_50);

    lv_style_init(&style_irrigation);
    lv_style_set_text_color(&style_irrigation, KODE_NEAR_BLACK);
    lv_style_set_text_font(&style_irrigation, &Inter_40);
    // Transparent label (container provides white background)
    lv_style_set_bg_opa(&style_irrigation, LV_OPA_TRANSP);
    lv_style_set_border_width(&style_irrigation, 0);
    lv_style_set_outline_width(&style_irrigation, 0);
    lv_style_set_radius(&style_irrigation, 0);
    lv_style_set_pad_left(&style_irrigation, 2);
    lv_style_set_pad_right(&style_irrigation, 2);
    lv_style_set_pad_top(&style_irrigation, 0);
    lv_style_set_pad_bottom(&style_irrigation, 0);
}

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
void createPlantMonitorUI();
void updatePlantAnimation();
void updateMoistureFluctuation();
void updateButtonInput();
void updatePlantImage();
static inline bool isPressed(uint8_t pinIndex);
static inline bool isGpioPressed(int gpio);

void setup() {
    // ---- Boot diagnostics ----
    Serial.begin(115200);
    Serial.println("Starting Plant Monitor System...");

    // Initialize display subsystem
    if (!display.init()) {
        Serial.println("Error: Failed to initialize display");
        while(1) {
            delay(1000);
        }
    }

    // Create the Plant Monitor UI
    setupBrandStyles();
    createPlantMonitorUI();

    // Configure TOP button (use internal pull-up for reliable reads)
    pinMode(BUTTON_TOP, INPUT_PULLUP);

    // Initialize LED manager
    LEDConfig ledCfg;
    ledCfg.pin = NEO_PIXEL_PIN;
    ledCfg.count = NEO_PIXEL_COUNT;
    ledCfg.brightness = 51; // ~20%
    
    if (!led.init(ledCfg)) {
        Serial.println("[LED] init failed");
    } else {
        Serial.println("[LED] initialized");
        // Set LED color based on initial moisture
        if (moisture_percentage >= 30.0f) {
            led.setPixelColor(0, 0, 255, 0); // Green for healthy
        } else {
            led.setPixelColor(0, 255, 0, 0); // Red for low moisture
        }
        led.show();
    }

    // ADC for soil (real mode uses ADC on GPIO13)
    pinMode(SOIL_PIN, INPUT);
    
    // Configure ADC like in working example
    Serial.println("Configurando ADC...");
    analogReadResolution(12); // 12-bit resolution
    analogSetAttenuation(ADC_11db); // 11dB attenuation for ~3.3V full scale

    // Initialize timing
    last_animation_time = millis();
    last_fluctuation_time = millis();

    Serial.printf("Plant Monitor ready! Starting moisture: %.1f%%  Mode=%s\n", moisture_percentage, use_real_soil ? "REAL" : "SIM");

    // Initialize PMIC/OTG for 5V power to pump + sensor
    initPmicForOtg();

    // Initialize IO Expander power rail (critical for motor driver power)
    if (ioexp.begin(INPUT)) {
        // Enable peripheral power high-side rail (matches example behavior)
        bool ok1 = ioexp.pinMode1(EXPANDER_PWR_PHRL, OUTPUT);
        bool ok2 = ioexp.write1(EXPANDER_PWR_PHRL, HIGH);
        Serial.printf("IOExp: PWR_PHRL OUT=%s, SET HIGH=%s\n", ok1 ? "OK" : "FAIL", ok2 ? "OK" : "FAIL");
        
        if (!ok1 || !ok2) {
            Serial.println("ERROR: Failed to enable peripheral power rail - motor may not work!");
        }
    } else {
        Serial.println("ERROR: IO Expander not connected - peripheral power rail unavailable!");
    }

    // Configure DRV8411A channels (matching working example)
    pinMode(PIN_AIN1, OUTPUT);
    pinMode(PIN_AIN2, OUTPUT);
    pinMode(PIN_BIN1, OUTPUT);
    pinMode(PIN_BIN2, OUTPUT);
    pinMode(PIN_nFAULT, INPUT_PULLUP);
    ledcAttach(PIN_AIN1, PWM_FREQ_HZ, PWM_RES_BITS);
    ledcAttach(PIN_AIN2, PWM_FREQ_HZ, PWM_RES_BITS);
    ledcAttach(PIN_BIN1, PWM_FREQ_HZ, PWM_RES_BITS);
    ledcAttach(PIN_BIN2, PWM_FREQ_HZ, PWM_RES_BITS);
    pump_coast();
    
    Serial.printf("Pump channel selected: %s\n", PUMP_USE_MOTOR_B ? "B (BIN1/BIN2)" : "A (AIN1/AIN2)");
    Serial.printf("Motor pins configured:\n");
    Serial.printf("  PIN_AIN1=%d, PIN_AIN2=%d\n", PIN_AIN1, PIN_AIN2);
    Serial.printf("  PIN_BIN1=%d, PIN_BIN2=%d\n", PIN_BIN1, PIN_BIN2);
    Serial.printf("  PIN_nFAULT=%d (current state: %s)\n", PIN_nFAULT, 
                 digitalRead(PIN_nFAULT) == HIGH ? "OK" : "FAULT");
    Serial.printf("  PWM_FREQ=%dHz, PWM_BITS=%d, PWM_MAX_DUTY=%u\n", 
                 PWM_FREQ_HZ, PWM_RES_BITS, PWM_MAX_DUTY);
}

void loop() {
    // Pump display subsystem (LVGL timers + rendering)
    display.update();
    
    // Update Plant Monitor components
    updateButtonInput();
    updateMoistureFluctuation();
    updatePlantAnimation();
    updatePmic();  // Update PMIC status
    
    // DRV8411A fault handling: stop sequence if nFAULT asserted
    static bool faultLatched = false;
    bool faultNow = (digitalRead(PIN_nFAULT) == LOW);
    if (faultNow && !faultLatched) {
        pump_coast();
        pump_seq_active = false;
        Serial.println("FAULT detected - pump stopped");
        faultLatched = true;
    } else if (!faultNow && faultLatched) {
        faultLatched = false;
        Serial.println("FAULT cleared");
    }

    // Simple pump sequence control - 5s forward only
    if (pump_seq_active) {
        uint32_t nowMs = millis();
        if (nowMs >= pump_seq_end_ms) {
            // Sequence complete - stop pump
            pump_coast();
            pump_seq_active = false;
            Serial.println("Pump: OFF (5s sequence done)");
            
            // Note: In SIM mode, the ramp was already started when pump began
            // so no need to start it here again
        }
    }
    // SIM ramp towards 83% after watering (10s irregular ramp with 2s delay)
    if (sim_ramp_active && !use_real_soil) {
        uint32_t nowMs = millis();
        uint32_t elapsed = nowMs - sim_ramp_start_ms;
        
        if (elapsed >= sim_ramp_delay_ms) {
            // Start growing after 2s delay
            uint32_t growth_time = elapsed - sim_ramp_delay_ms;
            
            if (growth_time >= sim_ramp_duration_ms) {
                // Ramp complete
                moisture_percentage = sim_ramp_to;
                sim_ramp_active = false;
                Serial.println("SIM: Moisture ramp completed");
            } else {
                // Natural irregular growth
                float base_progress = (float)growth_time / (float)sim_ramp_duration_ms; // 0..1
                
                // Add smooth irregular variations
                float noise_factor = 0.15f; // 15% variation
                float time_sec = growth_time / 1000.0f;
                
                // Generate smooth noise using multiple sine waves at different frequencies
                float noise1 = sinf(time_sec * 0.8f) * 0.4f;        // slow wave
                float noise2 = sinf(time_sec * 2.3f) * 0.3f;        // medium wave  
                float noise3 = sinf(time_sec * 4.7f) * 0.2f;        // fast wave
                float noise4 = sinf(time_sec * 0.3f) * 0.1f;        // very slow wave
                
                float combined_noise = (noise1 + noise2 + noise3 + noise4) / 4.0f;
                
                // Apply easing for more natural growth (fast start, slower end)
                float eased_progress = 1.0f - (1.0f - base_progress) * (1.0f - base_progress);
                
                // Add noise to the progress but keep it bounded
                float noisy_progress = eased_progress + (combined_noise * noise_factor * (1.0f - eased_progress));
                noisy_progress = fmaxf(0.0f, fminf(1.0f, noisy_progress));
                
                moisture_percentage = sim_ramp_from + (sim_ramp_to - sim_ramp_from) * noisy_progress;
                
                // Ensure we don't go backwards or exceed target
                static float last_moisture = sim_ramp_from;
                if (moisture_percentage < last_moisture) {
                    moisture_percentage = last_moisture + (moisture_percentage - last_moisture) * 0.1f; // gentle correction
                }
                if (moisture_percentage > sim_ramp_to) {
                    moisture_percentage = sim_ramp_to;
                }
                last_moisture = moisture_percentage;
            }
            
            // Update UI
            if (moisture_label) lv_label_set_text_fmt(moisture_label, "Soil: %d%%", (int)moisture_percentage);
            bool is_sad_now = (moisture_percentage < 30.0f);
            static bool was_sad = is_sad_now;
            if (was_sad != is_sad_now) { animation_frame = 0; updatePlantImage(); was_sad = is_sad_now; }
            if (moisture_percentage >= 30.0f) { led.setPixelColor(0, 0, 255, 0); } else { led.setPixelColor(0, 255, 0, 0); }
            led.show();
        }
    }
    
    delay(GUI_LOOP_DELAY_MS);
}

/**
 * @brief Create the Plant Monitor user interface
 *
 * Full screen plant image with overlaid title and moisture percentage
 */
void createPlantMonitorUI() {
    lv_obj_t * scr = lv_scr_act();
    // Apply brand background style
    lv_obj_add_style(scr, &style_screen_bg, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // Plant image (full screen background)
    plant_image = lv_image_create(scr);
    lv_image_set_src(plant_image, &Small);
    lv_obj_set_size(plant_image, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(plant_image, LV_OPA_TRANSP, 0);
    lv_obj_align(plant_image, LV_ALIGN_CENTER, 0, 0);

    // Title: "Plant Monitor" - positioned at top with light plant green color, moved down 10px
    title_label = lv_label_create(scr);
    lv_obj_add_style(title_label, &style_plant_title, 0);
    lv_label_set_text(title_label, "Plant Monitor");
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 40);

    // Moisture level container (auto-size to content to ensure it always fits)
    moisture_box = lv_obj_create(scr);
    lv_obj_set_size(moisture_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(moisture_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(moisture_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(moisture_box, 0, 0);
    lv_obj_set_style_outline_width(moisture_box, 0, 0);
    lv_obj_set_style_radius(moisture_box, 8, 0);
    lv_obj_set_style_pad_all(moisture_box, 6, 0);
    lv_obj_align(moisture_box, LV_ALIGN_BOTTOM_MID, 0, -30);

    // Moisture level display centered inside the container
    moisture_label = lv_label_create(moisture_box);
    lv_obj_add_style(moisture_label, &style_irrigation, 0);
    lv_label_set_text_fmt(moisture_label, "Soil: %d%%", (int)moisture_percentage);
    lv_label_set_long_mode(moisture_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(moisture_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(moisture_label);
}

/**
 * @brief Update plant animation based on moisture level
 */
void updatePlantAnimation() {
    static uint32_t last_update = 0;
    uint32_t now = millis();
    
    if (now - last_update < ANIMATION_UPDATE_MS) return;
    last_update = now;
    
    if (moisture_percentage >= 30.0f) {
        // Happy plant: Small -> Mid -> Big
        animation_frame = (animation_frame + 1) % 3;
    } else {
        // Sad plant: Sad -> VerySad
        animation_frame = (animation_frame + 1) % 2;
    }
    
    updatePlantImage();
}

/**
 * @brief Update the plant image based on moisture level and animation frame
 */
void updatePlantImage() {
    if (!plant_image) return;
    
    if (moisture_percentage >= 30.0f) {
        // Happy plant animation
        switch (animation_frame) {
            case 0: lv_image_set_src(plant_image, &Small); break;
            case 1: lv_image_set_src(plant_image, &Mid); break;
            case 2: lv_image_set_src(plant_image, &Big); break;
        }
    } else {
        // Sad plant animation
        switch (animation_frame) {
            case 0: lv_image_set_src(plant_image, &Sad); break;
            case 1: lv_image_set_src(plant_image, &VerySad); break;
        }
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

/**
 * @brief Handle button input for moisture control
 */
void updateButtonInput() {
    static uint32_t last_check = 0;
    static bool pad_up_was_pressed = false;
    static bool pad_down_was_pressed = false;
    static bool pad_left_was_pressed = false;
    static bool pad_right_was_pressed = false;
    static bool bottom_was_pressed = false;
    static bool top_was_pressed = false;
    
    uint32_t now = millis();
    if (now - last_check < BUTTON_CHECK_MS) return;
    last_check = now;
    
    bool pad_up_now = isPressed(EXPANDER_PAD_TOP);
    bool pad_down_now = isPressed(EXPANDER_PAD_BOTTOM);
    bool pad_left_now = isPressed(EXPANDER_PAD_LEFT);
    bool pad_right_now = isPressed(EXPANDER_PAD_RIGHT);
    bool bottom_now = isPressed(EXPANDER_BUTTON_BOTTOM);
    bool top_now = isGpioPressed(BUTTON_TOP);
    
    // Detect button press (rising edge)
    if (pad_up_now && !pad_up_was_pressed) {
        moisture_percentage = min(100.0f, moisture_percentage + 5.0f);
        Serial.printf("UP pressed - Moisture: %.1f%%\n", moisture_percentage);
        
        // Update UI immediately
        if (moisture_label) {
            lv_label_set_text_fmt(moisture_label, "Soil: %d%%", (int)moisture_percentage);
        }
        
        // Reset animation frame when switching between happy/sad
        static bool was_sad = false;
        bool is_sad_now = (moisture_percentage < 30.0f);
        if (was_sad != is_sad_now) {
            animation_frame = 0;
            updatePlantImage();
            was_sad = is_sad_now;
        }
        
        // Update LED color
        if (moisture_percentage >= 30.0f) {
            led.setPixelColor(0, 0, 255, 0); // Green for healthy
        } else {
            led.setPixelColor(0, 255, 0, 0); // Red for low moisture
        }
        led.show();
    }
    
    if (pad_down_now && !pad_down_was_pressed) {
        moisture_percentage = max(0.0f, moisture_percentage - 5.0f);
        Serial.printf("DOWN pressed - Moisture: %.1f%%\n", moisture_percentage);
        
        // Update UI immediately
        if (moisture_label) {
            lv_label_set_text_fmt(moisture_label, "Soil: %d%%", (int)moisture_percentage);
        }
        
        // Reset animation frame when switching between happy/sad
        static bool was_sad = false;
        bool is_sad_now = (moisture_percentage < 30.0f);
        if (was_sad != is_sad_now) {
            animation_frame = 0;
            updatePlantImage();
            was_sad = is_sad_now;
        }
        
        // Update LED color
        if (moisture_percentage >= 30.0f) {
            led.setPixelColor(0, 0, 255, 0); // Green for healthy
        } else {
            led.setPixelColor(0, 255, 0, 0); // Red for low moisture
        }
        led.show();
    }
    
    auto start_pump_sequence = [&]() {
        if (pump_seq_active) return; // ignore if already running
        if (sim_ramp_active) return; // ignore if ramp is active
        
        pump_seq_active = true;
        pump_forward_percent(100);
        pump_seq_end_ms = millis() + 5000;
        Serial.println("Pump: FORWARD 100% for 5s");
        
        if (!use_real_soil) {
            // In SIM mode, start moisture ramp immediately when pump starts
            sim_ramp_active = true;
            sim_ramp_from = moisture_percentage;
            sim_ramp_to = 83.0f;
            sim_ramp_start_ms = millis(); // start now, parallel to pump
            Serial.printf("SIM: Starting moisture ramp from %.1f%% to %.1f%% (2s delay + %ds irregular growth)\n", 
                         sim_ramp_from, sim_ramp_to, sim_ramp_duration_ms/1000);
        }
    };

    if (top_now && !top_was_pressed) { start_pump_sequence(); }
    if (pad_left_now && !pad_left_was_pressed) { start_pump_sequence(); }
    if (pad_right_now && !pad_right_was_pressed) { start_pump_sequence(); }

    // Toggle between SIM and REAL using the bottom button
    if (bottom_now && !bottom_was_pressed) {
        use_real_soil = !use_real_soil;
        Serial.printf("MODE toggled -> %s\n", use_real_soil ? "REAL" : "SIM");
    }

    pad_up_was_pressed = pad_up_now;
    pad_down_was_pressed = pad_down_now;
    pad_left_was_pressed = pad_left_now;
    pad_right_was_pressed = pad_right_now;
    bottom_was_pressed = bottom_now;
    top_was_pressed = top_now;
}

/**
 * @brief Add realistic moisture fluctuation
 */
void updateMoistureFluctuation() {
    static uint32_t last_update = 0;
    uint32_t now = millis();
    
    if (now - last_update < FLUCTUATION_UPDATE_MS) return;
    last_update = now;

    if (!use_real_soil) {
        if (sim_ramp_active) return; // suspend fluctuations during ramp
        // SIM mode: small random fluctuation (±0.5%)
        float fluctuation = (random(-50, 51) / 100.0f); // -0.5 to +0.5
        float new_moisture = moisture_percentage + fluctuation;
        new_moisture = max(0.0f, min(100.0f, new_moisture));
        if (abs(new_moisture - moisture_percentage) > 0.1f) {
            bool was_sad = (moisture_percentage < 30.0f);
            moisture_percentage = new_moisture;
            if (moisture_label) lv_label_set_text_fmt(moisture_label, "Soil: %d%%", (int)moisture_percentage);
            bool is_sad_now = (moisture_percentage < 30.0f);
            if (was_sad != is_sad_now) { animation_frame = 0; updatePlantImage(); }
            if (moisture_percentage >= 30.0f) { led.setPixelColor(0, 0, 255, 0); } else { led.setPixelColor(0, 255, 0, 0); }
            led.show();
        }
    } else {
        // REAL mode: read ADC and map to % with EMA filtering
        int raw = analogRead(SOIL_PIN);
        if (soil_filtered < 0.0f) soil_filtered = raw; // init filter
        soil_filtered = 0.8f * soil_filtered + 0.2f * raw; // EMA
        int pct = soilAnalogToPercent((int)(soil_filtered + 0.5f));
        float new_moisture = (float)pct;
        if (abs(new_moisture - moisture_percentage) > 0.5f) {
            bool was_sad = (moisture_percentage < 30.0f);
            moisture_percentage = new_moisture;
            if (moisture_label) lv_label_set_text_fmt(moisture_label, "Soil: %d%%", (int)moisture_percentage);
            bool is_sad_now = (moisture_percentage < 30.0f);
            if (was_sad != is_sad_now) { animation_frame = 0; updatePlantImage(); }
            if (moisture_percentage >= 30.0f) { led.setPixelColor(0, 0, 255, 0); } else { led.setPixelColor(0, 255, 0, 0); }
            led.show();
        }
    }
}

// End of Plant Monitor implementation