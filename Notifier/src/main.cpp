/*
 * Telegram Notification Display for Kode Dot (ESP32-S3)
 * ------------------------------------------------------------
 * Purpose
 *  - Connect to WiFi using WiFiManager
 *  - Poll Telegram for new messages
 *  - Display messages on screen
 *  - Move servo motor when message arrives
 *
 * Features covered
 *  - Display + LVGL v9 UI
 *  - WiFi connection via WiFiManager
 *  - HTTP requests to Telegram Bot API (getUpdates)
 *  - Servo motor control on GPIO13
 */
#include <Arduino.h>
#include <lvgl.h>
#include <kodedot/display_manager.h>
#include <kodedot/pin_config.h>
#include <wifi_manager_lib/WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <WiFi.h>
// NeoPixel LED manager
#include <led_manager/LEDManager.h>
// Brand logo image (generated C array)
extern const lv_image_dsc_t logotipo;

// ---- Brand fonts (Inter) ----
extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30;
extern const lv_font_t Inter_40;
extern const lv_font_t Inter_50;

// Telegram Bot Configuration
const char* BOT_TOKEN = "8482799063:AAHOFnijgCJEzKLhkKhfa4IvKQ_-RqA48fg";
const char* CHAT_ID = "8204393670";

// Servo configuration
#define SERVO_PIN 13
Servo servoMotor;

// Display manager instance
DisplayManager display;

// UI elements
static lv_obj_t *title_label;
static lv_obj_t *message_label;
static lv_obj_t *status_label;

// Message tracking
static int lastUpdateId = 0;
static String currentMessage = "No notifications";
static uint32_t lastPollTime = 0;
static const uint32_t POLL_INTERVAL_MS = 3000; // Poll every 3 seconds

// Typewriter effect state
static bool typeActive = false;
static String typeFullText = "";
static size_t typeIndex = 0;
static uint32_t typeLastMs = 0;
static const uint32_t TYPE_INTERVAL_MS = 80; // slower typing speed for nicer effect
static bool caretOn = true;
static uint32_t caretLastMs = 0;

// Servo frenzy state (non-blocking)
static bool servoFrenzyActive = false;
static bool servoHighNext = true;
static int servoToggles = 0; // 10 toggles = 5 cycles
static uint32_t servoLastToggleMs = 0;
static const int SERVO_REST = 0;     // Rest position at 0°
static const int SERVO_CENTER = 90;  // Notification center position at 90°
static const int SERVO_AMPLITUDE = 20; // small angle, frenetic
static const uint32_t SERVO_TOGGLE_INTERVAL_MS = 70; // fast
static const int SERVO_CYCLES = 12; // more cycles (was 5)

// Auto-hide notification after shown
static bool isMessageShowing = false;
static uint32_t messageShownAtMs = 0;
static const uint32_t MESSAGE_VISIBILITY_MS = 10000; // 10 seconds

// -----------------------------------------------------------------------------
// LED (NeoPixel) gentle breathing animation
// -----------------------------------------------------------------------------
static LEDManager led;
static bool ledReady = false;

enum class LedMode { Off, Connecting, Idle, Message, Error };
static LedMode ledMode = LedMode::Off;

// Animation parameters
static uint32_t ledPeriodMs = 3000;      // breathing period
static float ledMinScale = 0.02f;        // min intensity scale
static float ledMaxScale = 0.20f;        // max intensity scale
static uint8_t ledBaseR = 0, ledBaseG = 0, ledBaseB = 0; // base color
static uint32_t ledLastMs = 0;

static void setLedMode(LedMode mode) {
    ledMode = mode;
    switch (mode) {
        case LedMode::Connecting: // warm amber, medium pace
            ledBaseR = 255; ledBaseG = 180; ledBaseB = 80;
            ledPeriodMs = 2500; ledMinScale = 0.02f; ledMaxScale = 0.18f;
            break;
        case LedMode::Idle: // soft blue, slow and very gentle
            ledBaseR = 80; ledBaseG = 140; ledBaseB = 255;
            ledPeriodMs = 4200; ledMinScale = 0.01f; ledMaxScale = 0.12f;
            break;
        case LedMode::Message: // accent orange, a bit faster
            ledBaseR = 255; ledBaseG = 127; ledBaseB = 31; // KODE_ACCENT-ish
            ledPeriodMs = 1200; ledMinScale = 0.05f; ledMaxScale = 0.35f;
            break;
        case LedMode::Error: // red, slow
            ledBaseR = 255; ledBaseG = 0; ledBaseB = 0;
            ledPeriodMs = 3000; ledMinScale = 0.03f; ledMaxScale = 0.20f;
            break;
        case LedMode::Off:
        default:
            ledBaseR = 0; ledBaseG = 0; ledBaseB = 0;
            ledPeriodMs = 2000; ledMinScale = 0.0f; ledMaxScale = 0.0f;
            break;
    }
}

static void updateLed() {
    if (!ledReady) return;
    if (ledMode == LedMode::Off) {
        led.turnOff();
        led.show();
        return;
    }
    uint32_t now = millis();
    // Calculate breathing position 0..1 using sine wave
    float t = (ledPeriodMs > 0) ? (float)(now % ledPeriodMs) / (float)ledPeriodMs : 0.0f;
    float wave = 0.5f + 0.5f * sinf(2.0f * PI * t);
    float scale = ledMinScale + (ledMaxScale - ledMinScale) * wave;

    uint8_t r = (uint8_t)constrain((int)(ledBaseR * scale), 0, 255);
    uint8_t g = (uint8_t)constrain((int)(ledBaseG * scale), 0, 255);
    uint8_t b = (uint8_t)constrain((int)(ledBaseB * scale), 0, 255);
    led.setColor(r, g, b);
    led.show();
}

// -----------------------------------------------------------------------------
// Brand palette
// -----------------------------------------------------------------------------
static const lv_color_t KODE_BG_DARK        = lv_color_hex(0x000000);
static const lv_color_t KODE_TEXT_LIGHT     = lv_color_hex(0xFFFAF5);
static const lv_color_t KODE_TEXT_MUTED     = lv_color_hex(0x9A948F);
static const lv_color_t KODE_ACCENT         = lv_color_hex(0xFF7F1F);

// Global styles
static lv_style_t style_screen_bg;
static lv_style_t style_title;
static lv_style_t style_text;
static lv_style_t style_message;

static void setupBrandStyles() {
    lv_style_init(&style_screen_bg);
    lv_style_set_bg_color(&style_screen_bg, KODE_BG_DARK);

    lv_style_init(&style_title);
    lv_style_set_text_color(&style_title, KODE_TEXT_LIGHT);
    lv_style_set_text_font(&style_title, &Inter_30);

    lv_style_init(&style_text);
    lv_style_set_text_color(&style_text, KODE_TEXT_LIGHT);
    lv_style_set_text_font(&style_text, &Inter_30);

    lv_style_init(&style_message);
    lv_style_set_text_color(&style_message, KODE_TEXT_LIGHT);
    lv_style_set_text_font(&style_message, &Inter_30);
}

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
void createUserInterface();
void initWiFi();
void pollTelegramMessages();
void displayNewMessage(const String& message);
// Effects helpers (defined later)
static void startTypewriter(const String& text);
static void updateTypewriter();
static void startServoFrenzy();
static void updateServoFrenzy();

void setup() {
    // Initialize serial
    Serial.begin(115200);
    Serial.println("Starting Telegram Notifier...");

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
    
    // Force an initial LVGL draw so the user sees the status immediately
    for (int i = 0; i < 5; ++i) {
        display.update();
        delay(10);
    }
    
    // Initialize servo motor
    servoMotor.attach(SERVO_PIN, 500, 2400); // Attach servo to GPIO13
    servoMotor.write(SERVO_REST); // Start at rest position (0°)
    Serial.println("Servo initialized on GPIO13");

    // Initialize NeoPixel (very low brightness for subtle glow)
    LEDConfig ledCfg;
    ledCfg.pin = NEO_PIXEL_PIN;
    ledCfg.count = NEO_PIXEL_COUNT;
    ledCfg.brightness = 20; // ~8%
    ledReady = led.init(ledCfg);
    if (ledReady) {
        setLedMode(LedMode::Connecting);
    } else {
        Serial.println("LED init failed; continuing without LED effects");
    }

    // Initialize WiFi connection
    initWiFi();

    Serial.println("System ready!");
}

void loop() {
    // Pump display subsystem (LVGL timers + rendering)
    display.update();
    
    // Poll for new Telegram messages
    pollTelegramMessages();
    
    // Update effects
    updateTypewriter();
    updateServoFrenzy();
    updateLed();
    
    // Auto-hide notification after timeout
    if (isMessageShowing && (millis() - messageShownAtMs >= MESSAGE_VISIBILITY_MS)) {
        isMessageShowing = false;
        typeActive = false;
        currentMessage = "No notifications";
        if (message_label) lv_label_set_text(message_label, currentMessage.c_str());
        if (status_label) {
            lv_label_set_text(status_label, "Ready");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
        }
        // Return LED to idle breathing
        if (ledReady) setLedMode(LedMode::Idle);
    }
    
    delay(10);
}

/**
 * @brief Create the main user interface
 */
void createUserInterface() {
    lv_obj_t * scr = lv_scr_act();
    // Apply brand background style and disable scrolling
    lv_obj_add_style(scr, &style_screen_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_coord_t scr_w = lv_obj_get_width(scr);
    
    // Title at top
    title_label = lv_label_create(scr);
    lv_obj_add_style(title_label, &style_title, 0);
    lv_label_set_text(title_label, "NOTIFIER");
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

    // Subtle separator line under title
    lv_obj_t *sep = lv_obj_create(scr);
    lv_obj_set_size(sep, scr_w - 40, 2);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(sep, KODE_TEXT_MUTED, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 1, 0);
    
    // Message display (centered, with word wrap)
    message_label = lv_label_create(scr);
    lv_obj_add_style(message_label, &style_message, 0);
    lv_label_set_text(message_label, currentMessage.c_str());
    lv_label_set_long_mode(message_label, LV_LABEL_LONG_WRAP);
    // Expand to almost full screen width to reduce line breaks
    lv_obj_set_width(message_label, scr_w - 40); // 20px margin each side
    lv_obj_align(message_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(message_label, LV_TEXT_ALIGN_CENTER, 0);
    
    // Status at bottom
    status_label = lv_label_create(scr);
    lv_obj_add_style(status_label, &style_text, 0);
    lv_label_set_text(status_label, "Connecting...");
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_text_color(status_label, KODE_TEXT_MUTED, 0);
}


    // Start typewriter animation with given text
    static void startTypewriter(const String& text) {
        typeFullText = text;
        typeIndex = 0;
        typeActive = true;
        typeLastMs = millis();
        caretOn = true;
        caretLastMs = millis();
        if (message_label) lv_label_set_text(message_label, "");
    }

    // Update typewriter animation
    static void updateTypewriter() {
        if (!typeActive) return;
        uint32_t now = millis();

        // caret blink
        if (now - caretLastMs >= 400) {
            caretOn = !caretOn;
            caretLastMs = now;
        }

        if (now - typeLastMs >= TYPE_INTERVAL_MS) {
            typeLastMs = now;
            if (typeIndex < typeFullText.length()) {
                typeIndex++;
                String partial = typeFullText.substring(0, typeIndex);
                if (message_label) {
                    String withCaret = partial + (caretOn ? "|" : " ");
                    lv_label_set_text(message_label, withCaret.c_str());
                }
            } else {
                typeActive = false;
                if (message_label) lv_label_set_text(message_label, typeFullText.c_str());
            }
        }
    }

    // Start servo sequence: immediate 90° kick, then fast oscillation around 90°, finally return to 0°
    static void startServoFrenzy() {
        servoFrenzyActive = true;
        servoHighNext = true; // first toggle will go to +amplitude
        servoToggles = 0;
        // Immediate kick to 90° (center)
        servoMotor.write(SERVO_CENTER);
        servoLastToggleMs = millis();
    }

    static void updateServoFrenzy() {
        if (!servoFrenzyActive) return;
        uint32_t now = millis();
        if (now - servoLastToggleMs < SERVO_TOGGLE_INTERVAL_MS) return;
        servoLastToggleMs = now;

        int target = SERVO_CENTER + (servoHighNext ? SERVO_AMPLITUDE : -SERVO_AMPLITUDE);
        servoMotor.write(target);
        servoHighNext = !servoHighNext;
        servoToggles++;

        if (servoToggles >= (SERVO_CYCLES * 2)) { // two toggles per cycle
            // Return to rest position (0°) after frenzy
            servoMotor.write(SERVO_REST);
            servoFrenzyActive = false;
        }
    }


/**
 * @brief Initialize WiFi connection using WiFiManager
 */
void initWiFi() {
    Serial.println("Initializing WiFi...");
    
    // Ensure the user sees the connecting message on screen right away
    if (status_label) {
        lv_label_set_text(status_label, "Connecting...");
        lv_obj_set_style_text_color(status_label, KODE_TEXT_MUTED, 0);
    }
    // Flush UI before starting potentially blocking WiFi operations
    for (int i = 0; i < 3; ++i) {
        display.update();
        delay(10);
    }
    
    // Load WiFi credentials from SD card
    if (!wifiManager.loadCredentialsFromSD()) {
        Serial.println("Failed to load WiFi credentials from SD card");
        if (status_label) {
            lv_label_set_text(status_label, "No WiFi credentials");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
        }
        if (ledReady) setLedMode(LedMode::Error);
        return;
    }

    // Attempt to connect
    if (wifiManager.connectToWiFi()) {
        Serial.println("WiFi connected successfully!");
        if (status_label) {
            lv_label_set_text(status_label, "Ready");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
        }
        if (ledReady) setLedMode(LedMode::Idle);
    } else {
        Serial.println("Failed to connect to WiFi");
        if (status_label) {
            lv_label_set_text(status_label, "WiFi error");
            lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF0000), 0);
        }
        if (ledReady) setLedMode(LedMode::Error);
    }
}

/**
 * @brief Poll Telegram for new messages using getUpdates API
 */
void pollTelegramMessages() {
    uint32_t now = millis();
    
    // Only poll at specified interval
    if (now - lastPollTime < POLL_INTERVAL_MS) {
        return;
    }
    
    lastPollTime = now;
    
    if (!wifiManager.isConnected()) {
        return;
    }

    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure(); // Skip certificate validation
    
    // Build Telegram API URL for getUpdates
    String url = "https://api.telegram.org/bot";
    url += BOT_TOKEN;
    url += "/getUpdates?offset=";
    url += String(lastUpdateId + 1);
    url += "&limit=1";
    
    http.begin(client, url);
    
    int httpResponseCode = http.GET();
    
    if (httpResponseCode == 200) {
        String response = http.getString();
        
        // Parse JSON response
        DynamicJsonDocument doc(2048);
        deserializeJson(doc, response);
        
        JsonArray results = doc["result"];
        
        if (results.size() > 0) {
            // Get the latest message
            JsonObject update = results[0];
            int updateId = update["update_id"];
            
            if (updateId > lastUpdateId) {
                lastUpdateId = updateId;
                
                // Extract message text
                String messageText = "";
                if (update.containsKey("message")) {
                    messageText = update["message"]["text"].as<String>();
                } else if (update.containsKey("channel_post")) {
                    messageText = update["channel_post"]["text"].as<String>();
                }
                
                if (messageText.length() > 0) {
                    Serial.println("New message received: " + messageText);
                    displayNewMessage(messageText);
                }
            }
        }
    } else if (httpResponseCode > 0) {
        Serial.printf("HTTP Error: %d\n", httpResponseCode);
    }
    
    http.end();
}

/**
 * @brief Display new message on screen
 */
void displayNewMessage(const String& message) {
    currentMessage = message;
    
    if (status_label) {
        lv_label_set_text(status_label, "New message");
        lv_obj_set_style_text_color(status_label, KODE_ACCENT, 0);
    }
    if (ledReady) setLedMode(LedMode::Message);
    
    // Start fancy text + servo in parallel
    startTypewriter(currentMessage);
    startServoFrenzy();
    isMessageShowing = true;
    messageShownAtMs = millis();
}

// Blocking servo sweep removed; using startServoFrenzy + updateServoFrenzy