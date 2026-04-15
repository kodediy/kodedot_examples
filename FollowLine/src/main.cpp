/*
 * Line Following Robot for Kode Dot (ESP32-S3)
 * ------------------------------------------------------------
 * Purpose
 *  - Line following robot using TCRT5000 sensor and DRV8411A motor driver
 *  - Edge-following algorithm with visual feedback on display
 *  - Real-time telemetry and control via touchscreen interface
 *
 * Features covered
 *  - Display + LVGL v9 UI for robot status
 *  - Touch controls for start/stop/calibration
 *  - Addressable LED status indication
 *  - Line sensor (TCRT5000) on GPIO11
 *  - Dual DC motor control via DRV8411A
 *  - Real-time sensor and motor telemetry
 *
 * Line Following Logic
 *  - White surface -> turn left (CCW): Left slower, Right faster
 *  - Black line -> turn right (CW): Left faster, Right slower
 *  - Always forward motion (no reverse)
 */
#include <Arduino.h>
#include <math.h>
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
extern const lv_image_dsc_t HammyRobot;

// ---- Brand fonts (Inter) ----
// These symbols are provided by the generated font C files under src/fonts/
extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30;
extern const lv_font_t Inter_40;
extern const lv_font_t Inter_50;

// ---- Line Following Robot Configuration ----
/* Pines DRV8411A (Motor Driver) */
constexpr int PIN_AIN1   = 42;  // Motor A (derecha)
constexpr int PIN_AIN2   = 41;
constexpr int PIN_BIN1   = 39;  // Motor B (izquierda)
constexpr int PIN_BIN2   = 40;
constexpr int PIN_nFAULT = 3;

/* TCRT5000 analógico (Line Sensor) */
constexpr int PIN_LINE_AO = 11;

/* ---------- Parámetros del robot ---------- */
// Umbral fijo (mV): negro < THRESH_MV, blanco > THRESH_MV
constexpr int THRESH_MV = 500;
constexpr int HYST_MV   = 60;     // histéresis para evitar aleteo

// Velocidades (0..255 con resolución de 8 bits)
constexpr int BASE_PWM  = 90;     // avance base (debe ser > MIN_RUN_PWM_* + margen)
constexpr int TURN_PWM  = 50;     // tamaño del arco (diferencia de velocidad)

// Piso de PWM para vencer fricción/rozamiento
constexpr int MIN_RUN_PWM_A = 80; // rueda derecha (Motor A)
constexpr int MIN_RUN_PWM_B = 80; // rueda izquierda (Motor B)

/* ---------- PWM (LEDC por pin) ---------- */
constexpr int PWM_FREQ_HZ  = 8000;   // 8 kHz: buen par a bajas rpm
constexpr int PWM_RES_BITS = 8;      // 8 bits
constexpr int PWM_MAX      = (1 << PWM_RES_BITS) - 1;

// Robot state
enum RobotState {
    ROBOT_STOPPED,
    ROBOT_RUNNING,
    ROBOT_CALIBRATING,
    ROBOT_ERROR
};

static RobotState robotState = ROBOT_STOPPED;
static bool onBlack = false;
static int currentSensorValue = 0;
static int leftMotorPWM = 0;
static int rightMotorPWM = 0;

// Display manager instance
DisplayManager display;

// UI labels for line following robot
static lv_obj_t *robot_status_label;
static lv_obj_t *sensor_label;
static lv_obj_t *start_btn;
static lv_obj_t *stop_btn;
static lv_obj_t *calibrate_btn;

// IO Expander
static TCA9555 ioexp(IOEXP_I2C_ADDR);

// NeoPixel
static Adafruit_NeoPixel pixel(NEO_PIXEL_COUNT, NEO_PIXEL_PIN, LED_STRIP_COLOR_ORDER + LED_STRIP_TIMING);
static esp_pm_lock_handle_t g_no_light_sleep_lock = nullptr;

// Motor and sensor instances (kept for diagnostics)
static Adafruit_LSM6DSOX imu;
static Adafruit_LIS2MDL mag(12345);
static MAX31329 rtc;
static PMIC_BQ25896 bq;
static BQ27220 gauge;

// -----------------------------------------------------------------------------
// Update cadences (ms) – optimized for line following responsiveness
// -----------------------------------------------------------------------------
static const uint32_t GUI_LOOP_DELAY_MS   = 5;      // loop() delay – target ~200 Hz
static const uint32_t SENSOR_UPDATE_MS    = 10;     // Line sensor reading (100 Hz)
static const uint32_t MOTOR_UPDATE_MS     = 10;     // Motor control (100 Hz)
static const uint32_t STATUS_UPDATE_MS    = 100;    // UI status updates (10 Hz)
static const uint32_t LED_UPDATE_MS       = 250;    // LED status indication (4 Hz)

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
void createLineFollowingUI();
void updateRobotStatus();
void updateSensorReading();
void updateMotorControl();
void updateLEDStatus();
void handleTouchControls();

// Motor control functions
static inline void pwmAttachPin(int pin) { ledcAttach(pin, PWM_FREQ_HZ, PWM_RES_BITS); }
static inline void pwmWritePin(int pin, int duty) { ledcWrite(pin, constrain(duty, 0, PWM_MAX)); }

static inline void motorFwdA(int pwm) {
  int d = constrain(pwm, 0, PWM_MAX);
  if (d > 0 && d < MIN_RUN_PWM_A) d = MIN_RUN_PWM_A;   // piso rueda derecha
  pwmWritePin(PIN_AIN2, PWM_MAX);      // fijo a '1'
  pwmWritePin(PIN_AIN1, PWM_MAX - d);  // PWM invertido -> drive cuando está '0'
}

static inline void motorFwdB(int pwm) {
  int d = constrain(pwm, 0, PWM_MAX);
  if (d > 0 && d < MIN_RUN_PWM_B) d = MIN_RUN_PWM_B;   // piso rueda izquierda
  pwmWritePin(PIN_BIN1, PWM_MAX);      // fijo a '1'
  pwmWritePin(PIN_BIN2, PWM_MAX - d);  // PWM invertido -> drive cuando está '0'
}

static inline void motorFwdLR(int left, int right) {
  motorFwdB(left);   // Motor B = izquierda
  motorFwdA(right);  // Motor A = derecha
}

static inline void motorBrakeAll() {
  // Brake en ambas ruedas (INx=1, INy=1)
  pwmWritePin(PIN_AIN1, PWM_MAX); pwmWritePin(PIN_AIN2, PWM_MAX);
  pwmWritePin(PIN_BIN1, PWM_MAX); pwmWritePin(PIN_BIN2, PWM_MAX);
}

// Sensor reading function
static inline int readMilliVolts() {
  const int N = 4; long acc = 0;
  for (int i=0; i<N; ++i) { 
    acc += analogReadMilliVolts(PIN_LINE_AO); 
    delayMicroseconds(150); 
  }
  return (int)(acc / N);
}

// Helper functions for button reading
static bool isPressed(uint8_t pinIndex) {
    int v = ioexp.read1(pinIndex);
    return (v != TCA9555_INVALID_READ) && (v == LOW);
}

static bool isGpioPressed(int gpio) {
    return digitalRead(gpio) == LOW;
}

// Button event handlers
static void start_btn_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        robotState = ROBOT_RUNNING;
        Serial.println("Robot STARTED");
    }
}

static void stop_btn_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        robotState = ROBOT_STOPPED;
        motorBrakeAll();
        Serial.println("Robot STOPPED");
    }
}

static void calibrate_btn_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        robotState = ROBOT_CALIBRATING;
        Serial.println("Robot CALIBRATING");
    }
}

void setup() {
    // ---- Boot diagnostics ----
    Serial.begin(115200);
    Serial.println("Starting Line Following Robot...");

    // Initialize display subsystem
    if (!display.init()) {
        Serial.println("Error: Failed to initialize display");
        while(1) {
            delay(1000);
        }
    }

    // Create the line following UI
    setupBrandStyles();
    createLineFollowingUI();
    
    // Initialize motor driver pins
    pwmAttachPin(PIN_AIN1);
    pwmAttachPin(PIN_AIN2);
    pwmAttachPin(PIN_BIN1);
    pwmAttachPin(PIN_BIN2);
    pinMode(PIN_nFAULT, INPUT_PULLUP);
    
    // Initialize line sensor
    analogReadResolution(12); // 0..4095
    analogSetPinAttenuation(PIN_LINE_AO, ADC_11db); // ~0..3.3 V
    
    // Initialize IO Expander (inputs)
    if (!ioexp.begin(INPUT)) {
        Serial.println("Warning: IO Expander not connected");
    }

    // Configure TOP button (use internal pull-up for reliable reads)
    pinMode(BUTTON_TOP, INPUT_PULLUP);

    // Initialize NeoPixel
    if (!g_no_light_sleep_lock) {
        if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "neopixel", &g_no_light_sleep_lock) == ESP_OK) {
            esp_pm_lock_acquire(g_no_light_sleep_lock);
        }
    }
    pinMode(NEO_PIXEL_PIN, OUTPUT);
    pixel.begin();
    pixel.setBrightness(51); // ~20% brightness
    pixel.clear();
    pixel.show();

    // Start with robot stopped (LED off)
    pixel.setPixelColor(0, pixel.Color(0, 0, 0));
    pixel.show();

    // Motors stopped initially
    motorBrakeAll();

    // Initialize basic sensors for diagnostics
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    bq.begin();
    gauge.begin(Wire, 0x55, -1, -1, 400000);

    Serial.println("Line Following Robot ready!");
    Serial.println("Touch START to begin line following");
}

void loop() {
    // Pump display subsystem (LVGL timers + rendering)
    display.update();
    
    // Handle touch controls
    handleTouchControls();
    
    // Update sensor readings
    updateSensorReading();
    
    // Update motor control based on current state
    updateMotorControl();
    
    // Update robot status display
    updateRobotStatus();
    
    // Update LED status indication
    updateLEDStatus();
    
    delay(GUI_LOOP_DELAY_MS);
}

/**
 * @brief Create the line following robot user interface
 *
 * Clean UI showing robot status, sensor data, and control buttons.
 */
void createLineFollowingUI() {
    lv_obj_t * scr = lv_scr_act();
    // Apply brand background style and center content using flex layout
    lv_obj_add_style(scr, &style_screen_bg, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_row(scr, 4, 0);
    lv_obj_set_style_pad_top(scr, 10, 0);

    // Title
    lv_obj_t * title = lv_label_create(scr);
    lv_obj_add_style(title, &style_title, 0);
    lv_label_set_text(title, "LINE FOLLOWER");
    lv_obj_set_style_text_font(title, &Inter_30, 0);

    // HammyRobot logo
    lv_obj_t * img_logo = lv_image_create(scr);
    lv_image_set_src(img_logo, &HammyRobot);
    lv_obj_set_style_img_recolor_opa(img_logo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(img_logo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_margin_top(img_logo, 5, 0);

    // Content container
    lv_obj_t * content = lv_obj_create(scr);
    lv_obj_set_size(content, LV_PCT(90), LV_PCT(100));
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, 4, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Robot status
    robot_status_label = lv_label_create(content);
    lv_obj_add_style(robot_status_label, &style_accent, 0);
    lv_label_set_text(robot_status_label, "Status: STOPPED");
    lv_obj_set_style_text_font(robot_status_label, &Inter_30, 0);

    // Sensor reading
    sensor_label = lv_label_create(content);
    lv_obj_add_style(sensor_label, &style_text, 0);
    lv_label_set_text(sensor_label, "Sensor: --- mV (---)");
    lv_obj_set_style_text_font(sensor_label, &Inter_20, 0);

    // Control buttons container
    lv_obj_t * btn_container = lv_obj_create(content);
    lv_obj_set_size(btn_container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_container, 0, 0);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btn_container, 4, 0);
    lv_obj_set_style_pad_column(btn_container, 6, 0);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Start button
    start_btn = lv_btn_create(btn_container);
    lv_obj_set_size(start_btn, 100, 50);
    lv_obj_add_event_cb(start_btn, start_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * start_label = lv_label_create(start_btn);
    lv_label_set_text(start_label, "START");
    lv_obj_center(start_label);
    lv_obj_set_style_bg_color(start_btn, lv_color_hex(0x00AA00), 0);
    lv_obj_set_style_text_font(start_label, &Inter_20, 0);

    // Stop button
    stop_btn = lv_btn_create(btn_container);
    lv_obj_set_size(stop_btn, 100, 50);
    lv_obj_add_event_cb(stop_btn, stop_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * stop_label = lv_label_create(stop_btn);
    lv_label_set_text(stop_label, "STOP");
    lv_obj_center(stop_label);
    lv_obj_set_style_bg_color(stop_btn, lv_color_hex(0xAA0000), 0);
    lv_obj_set_style_text_font(stop_label, &Inter_20, 0);

    // Calibrate button
    calibrate_btn = lv_btn_create(btn_container);
    lv_obj_set_size(calibrate_btn, 100, 50);
    lv_obj_add_event_cb(calibrate_btn, calibrate_btn_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t * cal_label = lv_label_create(calibrate_btn);
    lv_label_set_text(cal_label, "CAL");
    lv_obj_center(cal_label);
    lv_obj_set_style_bg_color(calibrate_btn, lv_color_hex(0x0066AA), 0);
    lv_obj_set_style_text_font(cal_label, &Inter_20, 0);
}

void handleTouchControls() {
    // Handle physical button controls for manual override
    static bool topWasPressed = false;
    
    bool topNow = isGpioPressed(BUTTON_TOP);
    
    // Emergency stop on TOP button
    if (topNow && !topWasPressed) {
        robotState = ROBOT_STOPPED;
        motorBrakeAll();
        Serial.println("EMERGENCY STOP - TOP button pressed");
    }
    topWasPressed = topNow;

    // D-pad manual control when robot is stopped
    if (robotState == ROBOT_STOPPED) {
        int manualLeft = 0, manualRight = 0;
        
        if (isPressed(EXPANDER_PAD_TOP)) {
            // Forward
            manualLeft = manualRight = BASE_PWM;
        } else if (isPressed(EXPANDER_PAD_LEFT)) {
            // Turn left
            manualLeft = BASE_PWM - TURN_PWM;
            manualRight = BASE_PWM + TURN_PWM;
        } else if (isPressed(EXPANDER_PAD_RIGHT)) {
            // Turn right
            manualLeft = BASE_PWM + TURN_PWM;
            manualRight = BASE_PWM - TURN_PWM;
        }
        
        if (manualLeft != 0 || manualRight != 0) {
            motorFwdLR(manualLeft, manualRight);
            leftMotorPWM = manualLeft;
            rightMotorPWM = manualRight;
        } else {
            motorBrakeAll();
            leftMotorPWM = rightMotorPWM = 0;
        }
    }
}

void updateSensorReading() {
    static uint32_t lastUpdate = 0;
    uint32_t now = millis();
    if (now - lastUpdate < SENSOR_UPDATE_MS) return;
    lastUpdate = now;

    currentSensorValue = readMilliVolts();
    
    // Apply hysteresis
    if (!onBlack && (currentSensorValue < (THRESH_MV - HYST_MV))) {
        onBlack = true;
    } else if (onBlack && (currentSensorValue > (THRESH_MV + HYST_MV))) {
        onBlack = false;
    }
}

void updateMotorControl() {
    static uint32_t lastUpdate = 0;
    uint32_t now = millis();
    if (now - lastUpdate < MOTOR_UPDATE_MS) return;
    lastUpdate = now;

    // Check motor driver fault
    int nFault = digitalRead(PIN_nFAULT);
    if (nFault == LOW) {
        robotState = ROBOT_ERROR;
        motorBrakeAll();
        leftMotorPWM = rightMotorPWM = 0;
        Serial.println("[ERROR] Motor driver fault detected!");
        return;
    }

    if (robotState == ROBOT_RUNNING) {
        // Line following logic
        int L, R;
        if (onBlack) {
            // On black line -> turn right (CW)
            L = BASE_PWM + TURN_PWM;
            R = BASE_PWM - TURN_PWM;
        } else {
            // On white surface -> turn left (CCW)
            L = BASE_PWM - TURN_PWM;
            R = BASE_PWM + TURN_PWM;
        }
        
        // Apply constraints and set motors
        L = constrain(L, 0, PWM_MAX);
        R = constrain(R, 0, PWM_MAX);
        motorFwdLR(L, R);
        
        leftMotorPWM = L;
        rightMotorPWM = R;
        
        // Serial telemetry
        Serial.printf("%d\t%s\t%d\t%d\t%d\n", 
                     currentSensorValue, 
                     onBlack ? "BLACK" : "WHITE", 
                     L, R, nFault);
    } 
    else if (robotState == ROBOT_CALIBRATING) {
        // Simple calibration: test motors briefly
        static uint32_t calStart = 0;
        static int calPhase = 0;
        
        if (calStart == 0) calStart = now;
        
        uint32_t elapsed = now - calStart;
        
        if (elapsed < 1000) {
            // Phase 0: both motors forward
            motorFwdLR(BASE_PWM, BASE_PWM);
            leftMotorPWM = rightMotorPWM = BASE_PWM;
        } else if (elapsed < 2000) {
            // Phase 1: left turn
            motorFwdLR(BASE_PWM - TURN_PWM, BASE_PWM + TURN_PWM);
            leftMotorPWM = BASE_PWM - TURN_PWM;
            rightMotorPWM = BASE_PWM + TURN_PWM;
        } else if (elapsed < 3000) {
            // Phase 2: right turn
            motorFwdLR(BASE_PWM + TURN_PWM, BASE_PWM - TURN_PWM);
            leftMotorPWM = BASE_PWM + TURN_PWM;
            rightMotorPWM = BASE_PWM - TURN_PWM;
        } else {
            // Calibration complete
            robotState = ROBOT_STOPPED;
            motorBrakeAll();
            leftMotorPWM = rightMotorPWM = 0;
            calStart = 0;
            Serial.println("Calibration complete");
        }
    }
    else {
        // Robot stopped or error - ensure motors are off
        if (leftMotorPWM != 0 || rightMotorPWM != 0) {
            motorBrakeAll();
            leftMotorPWM = rightMotorPWM = 0;
        }
    }
}

void updateRobotStatus() {
    static uint32_t lastUpdate = 0;
    uint32_t now = millis();
    if (now - lastUpdate < STATUS_UPDATE_MS) return;
    lastUpdate = now;

    // Update robot status label
    const char* statusText = "UNKNOWN";
    switch(robotState) {
        case ROBOT_STOPPED: statusText = "STOPPED"; break;
        case ROBOT_RUNNING: statusText = "RUNNING"; break;
        case ROBOT_CALIBRATING: statusText = "CALIBRATING"; break;
        case ROBOT_ERROR: statusText = "ERROR"; break;
    }
    
    if (robot_status_label) {
        lv_label_set_text_fmt(robot_status_label, "Status: %s", statusText);
    }

    // Update sensor label
    if (sensor_label) {
        lv_label_set_text_fmt(sensor_label, "Sensor: %d mV (%s)", 
                             currentSensorValue, 
                             onBlack ? "BLACK" : "WHITE");
    }
}

void updateLEDStatus() {
    static uint32_t lastUpdate = 0;
    uint32_t now = millis();
    if (now - lastUpdate < LED_UPDATE_MS) return;
    lastUpdate = now;

    uint32_t color = pixel.Color(0, 0, 0); // Default off
    
    switch(robotState) {
        case ROBOT_STOPPED:
            // Slow blue pulse
            color = pixel.Color(0, 0, (sin(now * 0.003) + 1) * 127);
            break;
        case ROBOT_RUNNING:
            // Green when following line properly
            if (onBlack) {
                color = pixel.Color(0, 255, 0); // Bright green on line
            } else {
                color = pixel.Color(255, 255, 0); // Yellow when searching
            }
            break;
        case ROBOT_CALIBRATING:
            // Fast white blink
            color = pixel.Color(255, 255, 255);
            if ((now / 100) % 2 == 0) color = pixel.Color(0, 0, 0);
            break;
        case ROBOT_ERROR:
            // Fast red blink
            color = pixel.Color(255, 0, 0);
            if ((now / 200) % 2 == 0) color = pixel.Color(0, 0, 0);
            break;
    }
    
    pixel.setPixelColor(0, color);
    pixel.show();
}