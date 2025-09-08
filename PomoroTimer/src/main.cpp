/*
 * Pomodoro Timer for Kode Dot (ESP32-S3)
 * ------------------------------------------------------------
 * Purpose
 *  - A complete Pomodoro Timer with visual indicators and cycle management
 *  - Features work/break cycles with NeoPixel progress indication
 *  - Touch interface for time configuration and control
 *
 * Features
 *  - Work/break time configuration (1-60 minutes)
 *  - Visual timer with circular progress indicator
 *  - NeoPixel LED progress and state indication
 *  - Automatic cycle management (work -> break -> work...)
 *  - Touch controls for start/pause/reset
 *  - Sound notifications on cycle completion
 */
#include <Arduino.h>
#include <lvgl.h>
#include <kodedot/display_manager.h>
#include <TCA9555.h>
#include <kodedot/pin_config.h>
#include <led_manager/LEDManager.h>
#include <audio_manager/AudioManager.h>
#include <esp_pm.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Brand logo image (generated C array)
extern const lv_image_dsc_t logotipo;

// Brand fonts (Inter)
extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30;
extern const lv_font_t Inter_40;
extern const lv_font_t Inter_50;
extern const lv_font_t Inter_70;
extern const lv_font_t Inter_100;
extern const lv_font_t Inter_130;
extern const lv_font_t Inter_150;
extern const lv_font_t Inter_200;

// Display manager instance
DisplayManager display;

// IO Expander
static TCA9555 ioexp(IOEXP_I2C_ADDR);

// LED Manager
static LEDManager ledManager;

// Audio Manager
static AudioManager audioManager;

// Startup LED color cycle test removed
//#define LED_STARTUP_TEST 0

// -----------------------------------------------------------------------------
// Pomodoro Timer States and Variables
// -----------------------------------------------------------------------------
enum PomodoroState {
    STATE_CONFIG_WORK,    // Configuring work time
    STATE_CONFIG_BREAK,   // Configuring break time
        STATE_SOUND_CHOICE,   // Choosing sound on/off
    STATE_WORK,           // Work session
    STATE_BREAK,          // Break session
    STATE_FINISHED,
    STATE_PAUSED          // Timer paused
};

// Timer variables
static PomodoroState currentState = STATE_CONFIG_WORK;
static uint32_t workTimeMinutes = 25;      // Default: 25 minutes
static uint32_t breakTimeMinutes = 5;      // Default: 5 minutes
static uint32_t currentSessionMinutes = 25;
static uint32_t sessionStartTime = 0;
static uint32_t pausedTime = 0;
static uint32_t totalPausedTime = 0;
static int completedCycles = 0;
static bool isWorkSession = true;

// -----------------------------------------------------------------------------
// Update cadences (ms)
// -----------------------------------------------------------------------------
static const uint32_t GUI_LOOP_DELAY_MS = 50;      // 20 Hz for smooth animations
static const uint32_t TIMER_UPDATE_MS = 1000;      // Update timer every second
static const uint32_t LED_UPDATE_MS = 100;         // Update LED every 100ms

// -----------------------------------------------------------------------------
// Brand palette
// -----------------------------------------------------------------------------
static const lv_color_t KODE_BG_DARK        = lv_color_hex(0x000000); // background
static const lv_color_t KODE_TEXT_LIGHT     = lv_color_hex(0xFFD1BA); // timer display and title (warm orange/salmon)
static const lv_color_t KODE_TEXT_MUTED     = lv_color_hex(0x666666); // cycles text (dark gray)
static const lv_color_t KODE_ACCENT         = lv_color_hex(0xFF7F1F); // orange accent for buttons
static const lv_color_t KODE_PROGRESS_EMPTY = lv_color_hex(0xC3EAD5); // empty progress dots (light green)
static const lv_color_t KODE_PROGRESS_FILL  = lv_color_hex(0xFFD1BA); // filled progress dots (warm orange)
static const lv_color_t KODE_ACCENT_GREEN   = lv_color_hex(0x4CAF50); // green for break
// Work accent (was red) - use calm blue instead
static const lv_color_t KODE_ACCENT_RED     = lv_color_hex(0x2196F3); // blue for work

// Global styles
static lv_style_t style_screen_bg;
static lv_style_t style_title;
static lv_style_t style_text;
static lv_style_t style_large_text;
static lv_style_t style_button;
static lv_style_t style_cycles_text;
static lv_style_t style_blue_button;

// UI objects
static lv_obj_t *main_screen;
static lv_obj_t *config_work_screen;
static lv_obj_t *config_break_screen;
static lv_obj_t *timer_screen;
static lv_obj_t *sound_choice_screen; // Screen for sound enable choice

// Config work screen objects
static lv_obj_t *config_work_title_label;
static lv_obj_t *config_work_time_label;
static lv_obj_t *config_work_selector_bg;
static lv_obj_t *config_work_dots[12];  // 12 dots for visual selector
static lv_obj_t *config_work_ok_btn;

// Config break screen objects
static lv_obj_t *config_break_title_label;
static lv_obj_t *config_break_time_label;
static lv_obj_t *config_break_selector_bg;
static lv_obj_t *config_break_dots[12];  // 12 dots for visual selector
static lv_obj_t *config_break_ok_btn;
// Sound choice buttons
static lv_obj_t *sound_on_btn;
static lv_obj_t *sound_off_btn;

// Timer screen objects
static lv_obj_t *timer_title_label;
static lv_obj_t *timer_display_label;
static lv_obj_t *timer_cycles_label;
static lv_obj_t *progress_dots[20];  // 20 dots for arc progress (not full circle)
static lv_obj_t *cycle_dots_container = nullptr;
static lv_obj_t *cycle_dot_objs[5] = { nullptr };
static lv_obj_t *final_screen = nullptr;

// Forward declarations for new helpers
void updateCycleDots();
void createFinalScreen();

static void setupBrandStyles() {
    lv_style_init(&style_screen_bg);
    lv_style_set_bg_color(&style_screen_bg, KODE_BG_DARK);

    lv_style_init(&style_title);
    lv_style_set_text_color(&style_title, KODE_TEXT_LIGHT);
    lv_style_set_text_font(&style_title, &Inter_30);
    lv_style_set_text_align(&style_title, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&style_text);
    lv_style_set_text_color(&style_text, KODE_TEXT_MUTED);
    lv_style_set_text_font(&style_text, &Inter_20);
    lv_style_set_text_align(&style_text, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&style_large_text);
    lv_style_set_text_color(&style_large_text, KODE_TEXT_LIGHT);
    lv_style_set_text_font(&style_large_text, &Inter_100);  // Cambiado de Inter_130 a Inter_100
    lv_style_set_text_align(&style_large_text, LV_TEXT_ALIGN_CENTER);

    lv_style_init(&style_button);
    lv_style_set_bg_color(&style_button, KODE_ACCENT);
    lv_style_set_bg_opa(&style_button, LV_OPA_100);
    lv_style_set_text_color(&style_button, KODE_BG_DARK);
    lv_style_set_text_font(&style_button, &Inter_20);
    lv_style_set_radius(&style_button, 25);
    lv_style_set_border_width(&style_button, 0);

    // Cycles text style: slightly larger and darker
    lv_style_init(&style_cycles_text);
    lv_style_set_text_color(&style_cycles_text, KODE_TEXT_MUTED);
    lv_style_set_text_font(&style_cycles_text, &Inter_30); // un poco más grande que style_text
    lv_style_set_text_align(&style_cycles_text, LV_TEXT_ALIGN_CENTER);

    // Blue button style for configuration screens
    lv_style_init(&style_blue_button);
    lv_style_set_bg_color(&style_blue_button, KODE_PROGRESS_EMPTY); // Same as unmarked dots
    lv_style_set_bg_opa(&style_blue_button, LV_OPA_100);
    lv_style_set_text_color(&style_blue_button, KODE_BG_DARK);  // Dark text for readability
    lv_style_set_text_font(&style_blue_button, &Inter_30);
    lv_style_set_radius(&style_blue_button, 30);
    lv_style_set_border_width(&style_blue_button, 0);
}

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
void createConfigWorkScreen();
void createConfigBreakScreen();
void createTimerScreen();
void createSoundChoiceScreen();
static bool soundEnabled = false; // Set after user chooses
static bool completionHoldActive = false;
static uint32_t completionHoldStart = 0;
static bool soundPlayed = false; // Ensure sound plays only once per completion
void updateTimer();
void updateNeoPixel();
void handleTouch();
void startTimer();
void pauseTimer();
void resetTimer();
void switchToNextSession();
uint32_t getElapsedMillis();
uint32_t getElapsedSeconds();
uint32_t getRemainingSeconds();
bool isTimerComplete();
void updateConfigWorkSelector();
void updateConfigBreakSelector();
void startConfiguredTimer();
void playCompletionSound(bool isWorkComplete);

// -----------------------------------------------------------------------------
// Touch handling helpers
// -----------------------------------------------------------------------------
static inline bool isPressed(uint8_t pinIndex) {
    int v = ioexp.read1(pinIndex);
    return (v != TCA9555_INVALID_READ) && (v == LOW);
}

static inline bool isGpioPressed(int gpio) {
    return digitalRead(gpio) == LOW;
}

void setup() {
    // ---- Boot diagnostics ----
    Serial.begin(115200);
    Serial.println("Starting Pomodoro Timer...");

    // Disable power management early to avoid conflicts between I2S and RMT
    esp_pm_config_t pm_config;
    pm_config.max_freq_mhz = 240;
    pm_config.min_freq_mhz = 240;
    pm_config.light_sleep_enable = false;
    esp_err_t pm_err = esp_pm_configure(&pm_config);
    if (pm_err == ESP_OK) {
        Serial.println("Power management configured - light sleep disabled");
    } else {
        Serial.printf("Failed to configure power management: %d\n", pm_err);
    }

    // Initialize display subsystem
    if (!display.init()) {
        Serial.println("Error: Failed to initialize display");
        while(1) {
            delay(1000);
        }
    }

    // Create the UI
    setupBrandStyles();
    createConfigWorkScreen();
    createConfigBreakScreen();
    createTimerScreen();
        createSoundChoiceScreen();
    
    // Start with work config screen
    lv_scr_load(config_work_screen);
    
    // Initialize IO Expander (inputs)
    if (!ioexp.begin(INPUT)) {
        Serial.println("Warning: IO Expander not connected");
    }

    // Configure TOP button (use internal pull-up for reliable reads)
    pinMode(BUTTON_TOP, INPUT_PULLUP);

    // Initialize LED Manager
    LEDConfig ledConfig;
    ledConfig.pin = NEO_PIXEL_PIN;
    ledConfig.count = NEO_PIXEL_COUNT;
    // Keep existing base brightness value as before. We'll treat this value as 100%
    // for the new percent-based API below, so callers can set 0..100 relative to this.
    ledConfig.brightness = 51; // base (treated as 100% for percent API)
    #ifdef LED_USE_NEOPIXELBUS
    ledConfig.colorOrder = 0; // Not used by NeoPixelBus bitbang implementation
    #else
    ledConfig.colorOrder = LED_STRIP_COLOR_ORDER + LED_STRIP_TIMING;
    #endif
    
    if (!ledManager.init(ledConfig)) {
        Serial.println("Error: Failed to initialize LED Manager");
    } else {
        // Set initial orange (config mode) only
    // Reduce initial brightness to 50% of the baseBrightness (51) so LEDs are dimmer
    ledManager.setBrightnessPercent(50);
    ledManager.setColor(255, 127, 31);
        Serial.println("LED Manager initialized");
        ledManager.show();
    }
    
    // Initialize Audio Manager with speaker configuration
    AudioConfig audioConfig;
    audioConfig.spkSckPin = SPK_I2S_SCK;
    audioConfig.spkWsPin = SPK_I2S_WS;
    audioConfig.spkDoutPin = SPK_I2S_DOUT;
    
    if (!audioManager.init(audioConfig)) {
        Serial.println("Warning: Failed to initialize Audio Manager");
    } else {
        Serial.println("Audio Manager initialized");
        audioManager.attachExpander(&ioexp, EXPANDER_SPK_SHUTDOWN);
    }

    Serial.println("Pomodoro Timer ready!");
}

void loop() {
    // Pump display subsystem (LVGL timers + rendering)
    display.update();
    
    // Handle touch input
    handleTouch();
    
    // Update timer logic
    updateTimer();
    
    // Update NeoPixel (includes show() call internally)
    updateNeoPixel();

    // LED show() is now handled inside updateNeoPixel() to avoid conflicts
    // ledManager.show();
    
    delay(GUI_LOOP_DELAY_MS);
}

// -----------------------------------------------------------------------------
// Configuration Work Screen Creation
// -----------------------------------------------------------------------------
void createConfigWorkScreen() {
    config_work_screen = lv_obj_create(NULL);
    lv_obj_add_style(config_work_screen, &style_screen_bg, 0);
    // Ensure config screen doesn't show scroll/slide indicators
    lv_obj_clear_flag(config_work_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(config_work_screen, LV_SCROLLBAR_MODE_OFF);
    
    // Title
    config_work_title_label = lv_label_create(config_work_screen);
    lv_obj_add_style(config_work_title_label, &style_title, 0);
    lv_label_set_text(config_work_title_label, "WORK TIME");
    lv_obj_align(config_work_title_label, LV_ALIGN_TOP_MID, 0, 30); // Misma posición que timer principal
    
    // Time display
    config_work_time_label = lv_label_create(config_work_screen);
    lv_obj_add_style(config_work_time_label, &style_large_text, 0);
    lv_obj_align(config_work_time_label, LV_ALIGN_CENTER, 0, 10);  // Misma posición que timer principal
    
    // Create selector dots (12 positions like a clock) - NO background circle
    for (int i = 0; i < 12; i++) {
        config_work_dots[i] = lv_obj_create(config_work_screen);
        lv_obj_set_size(config_work_dots[i], 24, 24);  // Igual que progress dots principales
        lv_obj_set_style_radius(config_work_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(config_work_dots[i], 0, 0);
        lv_obj_set_style_bg_color(config_work_dots[i], KODE_PROGRESS_EMPTY, 0);
    lv_obj_set_style_bg_opa(config_work_dots[i], LV_OPA_60, 0);
    // Disable any scroll/slide indicators for this dot
    lv_obj_clear_flag(config_work_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(config_work_dots[i], LV_SCROLLBAR_MODE_OFF);
    // Position dots around the circle - same radius and center as main screen
        float angle = (i * 30 - 90) * M_PI / 180; // Start from top (12 o'clock)
        const float baseRadius = 168.0f * 1.05f;  // Same as main screen
        int x = (int)(baseRadius * cos(angle));
        int y = (int)(baseRadius * sin(angle));
        lv_obj_align(config_work_dots[i], LV_ALIGN_CENTER, x, y + 10);  // Same offset as main
    }
    
    // OK button
    config_work_ok_btn = lv_btn_create(config_work_screen);
    lv_obj_add_style(config_work_ok_btn, &style_blue_button, 0);
    lv_obj_set_size(config_work_ok_btn, 180, 60);  // Más grande
    lv_obj_align(config_work_ok_btn, LV_ALIGN_BOTTOM_MID, 0, -110); // Subido 60px
    
    lv_obj_t * btn_label = lv_label_create(config_work_ok_btn);
    lv_label_set_text(btn_label, "OK");
    lv_obj_center(btn_label);
    
    updateConfigWorkSelector();
}

// -----------------------------------------------------------------------------
// Configuration Break Screen Creation
// -----------------------------------------------------------------------------
void createConfigBreakScreen() {
    config_break_screen = lv_obj_create(NULL);
    lv_obj_add_style(config_break_screen, &style_screen_bg, 0);
    lv_obj_clear_flag(config_break_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(config_break_screen, LV_SCROLLBAR_MODE_OFF);
    
    // Title
    config_break_title_label = lv_label_create(config_break_screen);
    lv_obj_add_style(config_break_title_label, &style_title, 0);
    lv_label_set_text(config_break_title_label, "BREAK TIME");
    lv_obj_align(config_break_title_label, LV_ALIGN_TOP_MID, 0, 30); // Misma posición que timer principal
    
    // Time display
    config_break_time_label = lv_label_create(config_break_screen);
    lv_obj_add_style(config_break_time_label, &style_large_text, 0);
    lv_obj_align(config_break_time_label, LV_ALIGN_CENTER, 0, 10);  // Misma posición que timer principal
    
    // Create selector dots (12 positions like a clock) - NO background circle
    for (int i = 0; i < 12; i++) {
        config_break_dots[i] = lv_obj_create(config_break_screen);
        lv_obj_set_size(config_break_dots[i], 24, 24);  // Igual que progress dots principales
        lv_obj_set_style_radius(config_break_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(config_break_dots[i], 0, 0);
        lv_obj_set_style_bg_color(config_break_dots[i], KODE_PROGRESS_EMPTY, 0);
    lv_obj_set_style_bg_opa(config_break_dots[i], LV_OPA_60, 0);
    // Disable any scroll/slide indicators for this dot
    lv_obj_clear_flag(config_break_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(config_break_dots[i], LV_SCROLLBAR_MODE_OFF);
    // Position dots around the circle - only top half for break times (1-30 min)
        if (i < 6) { // Only first 6 dots for break times (5, 10, 15, 20, 25, 30)
            float angle = (i * 30 - 90) * M_PI / 180; // Start from top (12 o'clock)
            const float baseRadius = 168.0f * 1.05f;  // Same as main screen
            int x = (int)(baseRadius * cos(angle));
            int y = (int)(baseRadius * sin(angle));
            lv_obj_align(config_break_dots[i], LV_ALIGN_CENTER, x, y + 10);  // Same offset as main
        } else {
            lv_obj_add_flag(config_break_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    // START button
    config_break_ok_btn = lv_btn_create(config_break_screen);
    lv_obj_add_style(config_break_ok_btn, &style_blue_button, 0);
    lv_obj_set_size(config_break_ok_btn, 180, 60);  // Más grande
    lv_obj_align(config_break_ok_btn, LV_ALIGN_BOTTOM_MID, 0, -110); // Subido 60px
    
    lv_obj_t * btn_label = lv_label_create(config_break_ok_btn);
    lv_label_set_text(btn_label, "START");
    lv_obj_center(btn_label);
    
    updateConfigBreakSelector();
}

// -----------------------------------------------------------------------------
// Sound Choice Screen Creation
// -----------------------------------------------------------------------------
void createSoundChoiceScreen() {
    sound_choice_screen = lv_obj_create(NULL);
    lv_obj_add_style(sound_choice_screen, &style_screen_bg, 0);
    lv_obj_clear_flag(sound_choice_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(sound_choice_screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t * title = lv_label_create(sound_choice_screen);
    lv_obj_add_style(title, &style_title, 0);
    lv_label_set_text(title, "SOUND?");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    sound_on_btn = lv_btn_create(sound_choice_screen);
    lv_obj_add_style(sound_on_btn, &style_blue_button, 0);
    lv_obj_set_size(sound_on_btn, 160, 80);
    lv_obj_align(sound_on_btn, LV_ALIGN_CENTER, -110, 80);
    lv_obj_t * on_lbl = lv_label_create(sound_on_btn);
    lv_label_set_text(on_lbl, "ON");
    lv_obj_center(on_lbl);

    sound_off_btn = lv_btn_create(sound_choice_screen);
    lv_obj_add_style(sound_off_btn, &style_blue_button, 0);
    lv_obj_set_size(sound_off_btn, 160, 80);
    lv_obj_align(sound_off_btn, LV_ALIGN_CENTER, 110, 80);
    lv_obj_t * off_lbl = lv_label_create(sound_off_btn);
    lv_label_set_text(off_lbl, "OFF");
    lv_obj_center(off_lbl);

    // Instruction label
    lv_obj_t * info = lv_label_create(sound_choice_screen);
    lv_obj_add_style(info, &style_text, 0);
    lv_label_set_text(info, "Choose if you want sound\nThen the timer will start");
    lv_obj_align(info, LV_ALIGN_CENTER, 0, -20);
}

static void showSoundChoice() {
    currentState = STATE_SOUND_CHOICE;
    lv_scr_load(sound_choice_screen);
}

// -----------------------------------------------------------------------------
// Timer Screen Creation
// -----------------------------------------------------------------------------
void createTimerScreen() {
    timer_screen = lv_obj_create(NULL);
    lv_obj_add_style(timer_screen, &style_screen_bg, 0);
    lv_obj_clear_flag(timer_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(timer_screen, LV_SCROLLBAR_MODE_OFF);
    
    // Title
    timer_title_label = lv_label_create(timer_screen);
    lv_obj_add_style(timer_title_label, &style_title, 0);
    lv_label_set_text(timer_title_label, "POMODORO\nTIMER");
    lv_obj_align(timer_title_label, LV_ALIGN_TOP_MID, 0, 30); // Subido 20px
    
        // Timer display (moved down 10px)
    timer_display_label = lv_label_create(timer_screen);
    lv_obj_add_style(timer_display_label, &style_large_text, 0);
    lv_obj_align(timer_display_label, LV_ALIGN_CENTER, 0, 20);  // Bajado 10px: de 10 a 20
    // Ensure the label has a proper initial value to avoid LVGL default "text" flash
    lv_label_set_text_fmt(timer_display_label, "%d:%02d", currentSessionMinutes, 0);
    
    // Create progress dots (20 positions in 3/4 circle - missing bottom quarter)
    // Arc goes 270 degrees: from top-left around to top-right, leaving bottom empty
    for (int i = 0; i < 20; i++) {
        progress_dots[i] = lv_obj_create(timer_screen);
        lv_obj_set_size(progress_dots[i], 24, 24);  // El doble: de 12x12 a 24x24
        lv_obj_set_style_radius(progress_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(progress_dots[i], 0, 0);
    lv_obj_set_style_bg_color(progress_dots[i], KODE_PROGRESS_EMPTY, 0);
    lv_obj_set_style_bg_opa(progress_dots[i], LV_OPA_100, 0);
    // Ensure progress dots do not show slide indicators
    lv_obj_clear_flag(progress_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(progress_dots[i], LV_SCROLLBAR_MODE_OFF);
        
        // Position dots in 3/4 circle: 270 degrees starting from top-left
        // Start at 135° (top-left) and go 270° clockwise to 45° (top-right)
    float startAngle = 135; // Start from top-left
    float arcSpan = 270;    // 3/4 circle (270 degrees)

    float angle = (startAngle + (i * arcSpan / (20 - 1))) * M_PI / 180;
    // Previous radius was 168 (20% bigger than 140). Increase it by 5% as requested.
    const float baseRadius = 168.0f * 1.05f;
    int x = (int)(baseRadius * cos(angle));
    int y = (int)(baseRadius * sin(angle));
    lv_obj_align(progress_dots[i], LV_ALIGN_CENTER, x, y + 20);  // Bajado 10px: de +10 a +20
    }
    
    // Cycles display (moved down 10px)
    timer_cycles_label = lv_label_create(timer_screen);
    lv_obj_add_style(timer_cycles_label, &style_cycles_text, 0);
    lv_obj_align(timer_cycles_label, LV_ALIGN_CENTER, 0, 170);  // Bajado 10px: de 160 a 170
    // Initialize cycles label to avoid default text
    lv_label_set_text_fmt(timer_cycles_label, "%d CYCLES", completedCycles);
    
    // Create cycle indicator dots (5 dots for completed cycles)
    cycle_dots_container = lv_obj_create(timer_screen);
    lv_obj_set_size(cycle_dots_container, 200, 30);  // Más ancho para acomodar bolas más grandes
    lv_obj_set_style_bg_opa(cycle_dots_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cycle_dots_container, 0, 0);
    lv_obj_align(cycle_dots_container, LV_ALIGN_CENTER, 0, 200);  // Bajado 10px: de 190 a 200
    // Disable any scroll indicators or slide behaviour
    lv_obj_clear_flag(cycle_dots_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cycle_dots_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(cycle_dots_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cycle_dots_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cycle_dots_container, 20, 0);  // Más separación entre bolas
    
    for (int i = 0; i < 5; i++) {
        lv_obj_t * dot = lv_obj_create(cycle_dots_container);
        lv_obj_set_size(dot, 18, 18);  // Un poco más grandes las bolas de ciclos
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, KODE_PROGRESS_EMPTY, 0);  // Color inicial vacío
        lv_obj_set_style_bg_opa(dot, LV_OPA_100, 0);
        // Disable slide indicators for cycle dots
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(dot, LV_SCROLLBAR_MODE_OFF);
        // store reference
        cycle_dot_objs[i] = dot;
    }

    // No buttons - touch center to pause/resume
}

// -----------------------------------------------------------------------------
// Pomodoro Timer Logic Functions
// -----------------------------------------------------------------------------

void updateConfigWorkSelector() {
    if (!config_work_time_label) return;
    
    // Update time display
    lv_label_set_text_fmt(config_work_time_label, "%d:00", workTimeMinutes);
    
    // Update dots to show current selection (like a clock)
    for (int i = 0; i < 12; i++) {
        // Convert minutes to clock position (5min intervals: 5,10,15...60)
        int timeForPosition = (i + 1) * 5;
        if (timeForPosition == workTimeMinutes) {
            lv_obj_set_style_bg_color(config_work_dots[i], KODE_PROGRESS_FILL, 0);
            lv_obj_set_style_bg_opa(config_work_dots[i], LV_OPA_100, 0);
        } else {
            lv_obj_set_style_bg_color(config_work_dots[i], KODE_PROGRESS_EMPTY, 0);
            lv_obj_set_style_bg_opa(config_work_dots[i], LV_OPA_60, 0);
        }
    }
}

void updateConfigBreakSelector() {
    if (!config_break_time_label) return;
    
    // Update time display
    lv_label_set_text_fmt(config_break_time_label, "%d:00", breakTimeMinutes);
    
    // Update dots to show current selection (only first 6 dots for 5-30 min)
    for (int i = 0; i < 6; i++) {
        // Convert minutes to clock position (5min intervals: 5,10,15,20,25,30)
        int timeForPosition = (i + 1) * 5;
        if (timeForPosition == breakTimeMinutes) {
            lv_obj_set_style_bg_color(config_break_dots[i], KODE_PROGRESS_FILL, 0);
            lv_obj_set_style_bg_opa(config_break_dots[i], LV_OPA_100, 0);
        } else {
            lv_obj_set_style_bg_color(config_break_dots[i], KODE_PROGRESS_EMPTY, 0);
            lv_obj_set_style_bg_opa(config_break_dots[i], LV_OPA_60, 0);
        }
    }
}

void handleTouch() {
    int16_t x, y;
    static bool wasTouched = false;
    static uint32_t lastTouchTime = 0;
    bool isTouched = display.getTouchCoordinates(x, y);
    uint32_t now = millis();
    if (isTouched && !wasTouched && (now - lastTouchTime > 180)) {
        lastTouchTime = now;
        if (currentState == STATE_CONFIG_WORK) {
            if (config_work_ok_btn) { lv_area_t a; lv_obj_get_click_area(config_work_ok_btn, &a); if (x>=a.x1&&x<=a.x2&&y>=a.y1&&y<=a.y2) { if (soundEnabled) audioManager.playBeep(1500,70,5000); currentState=STATE_CONFIG_BREAK; lv_scr_load(config_break_screen); return; } }
            if (y>150 && y<350) { workTimeMinutes+=5; if (workTimeMinutes>60) workTimeMinutes=5; updateConfigWorkSelector(); }
        } else if (currentState == STATE_CONFIG_BREAK) {
            if (config_break_ok_btn) { lv_area_t a; lv_obj_get_click_area(config_break_ok_btn, &a); if (x>=a.x1&&x<=a.x2&&y>=a.y1&&y<=a.y2) { if (soundEnabled) audioManager.playBeep(1800,80,6000); currentState=STATE_SOUND_CHOICE; lv_scr_load(sound_choice_screen); return; } }
            if (y>150 && y<350) { breakTimeMinutes+=5; if (breakTimeMinutes>30) breakTimeMinutes=5; updateConfigBreakSelector(); }
        } else if (currentState == STATE_SOUND_CHOICE) {
            if (sound_on_btn) { lv_area_t a; lv_obj_get_click_area(sound_on_btn,&a); if (x>=a.x1&&x<=a.x2&&y>=a.y1&&y<=a.y2) { soundEnabled=true; if (soundEnabled) audioManager.playBeep(1600,60,6000); startConfiguredTimer(); return; } }
            if (sound_off_btn) { lv_area_t a; lv_obj_get_click_area(sound_off_btn,&a); if (x>=a.x1&&x<=a.x2&&y>=a.y1&&y<=a.y2) { soundEnabled=false; startConfiguredTimer(); return; } }
        } else if (currentState == STATE_WORK || currentState == STATE_BREAK) {
            if (x>100 && x<310 && y>200 && y<350) { pauseTimer(); }
        } else if (currentState == STATE_PAUSED) {
            if (x>100 && x<310 && y>200 && y<350) { startTimer(); }
        }
    }
    wasTouched = isTouched;
}

void startConfiguredTimer() {
    currentState = STATE_WORK;
    isWorkSession = true;
    currentSessionMinutes = workTimeMinutes;
    completedCycles = 0;
    sessionStartTime = millis();
    totalPausedTime = 0;
    
    // Switch to timer screen
    lv_scr_load(timer_screen);
    // Set title to WORKING
    if (timer_title_label) lv_label_set_text(timer_title_label, "WORKING");
    
    Serial.printf("Starting Pomodoro: Work=%dmin, Break=%dmin\n", workTimeMinutes, breakTimeMinutes);
}

void startTimer() {
    if (currentState == STATE_PAUSED) {
        // Resume from pause
        totalPausedTime += millis() - pausedTime;
        currentState = isWorkSession ? STATE_WORK : STATE_BREAK;
    Serial.println("Timer resumed");
    }
}

void pauseTimer() {
    if (currentState == STATE_WORK || currentState == STATE_BREAK) {
        currentState = STATE_PAUSED;
        pausedTime = millis();
    Serial.println("Timer paused");
    }
}

void resetTimer() {
    currentState = STATE_CONFIG_WORK;
    totalPausedTime = 0;
    pausedTime = 0;
    
    // Switch back to work config screen
    lv_scr_load(config_work_screen);
    
    Serial.println("Timer reset - back to config");
}

void switchToNextSession() {
    if (isWorkSession) {
        // Work session completed, start break
        isWorkSession = false;
        currentSessionMinutes = breakTimeMinutes;
        completedCycles++;
        // Update visual cycle indicators
        updateCycleDots();
        // If we've completed 5 cycles, show the final screen and stop
        if (completedCycles >= 5) {
            createFinalScreen();
            lv_scr_load(final_screen);
            currentState = STATE_FINISHED;
            Serial.println("All 5 cycles completed - final screen shown");
            return; // stop further processing
        }
        currentState = STATE_BREAK;
        Serial.printf("Work session completed! Starting break (%d min). Cycles: %d\n", breakTimeMinutes, completedCycles);
    if (timer_title_label) lv_label_set_text(timer_title_label, "RESTING");
    } else {
        // Break completed, start work
        isWorkSession = true;
        currentSessionMinutes = workTimeMinutes;
        currentState = STATE_WORK;
        Serial.printf("Break completed! Starting work (%d min)\n", workTimeMinutes);
    if (timer_title_label) lv_label_set_text(timer_title_label, "WORKING");
    }
    
    // Reset timer for new session
    sessionStartTime = millis();
    totalPausedTime = 0;
    // No pause indicator to hide (LED blink used)
}

// Update the cycle indicator dots according to completedCycles
void updateCycleDots() {
    for (int i = 0; i < 5; i++) {
        if (!cycle_dot_objs[i]) continue;
        if (i < completedCycles) {
            lv_obj_set_style_bg_color(cycle_dot_objs[i], KODE_PROGRESS_FILL, 0);
            lv_obj_set_style_bg_opa(cycle_dot_objs[i], LV_OPA_100, 0);
        } else {
            lv_obj_set_style_bg_color(cycle_dot_objs[i], KODE_PROGRESS_EMPTY, 0);
            lv_obj_set_style_bg_opa(cycle_dot_objs[i], LV_OPA_100, 0);
        }
    }
}

// Create a final screen telling the user they completed 5 cycles
void createFinalScreen() {
    if (final_screen) return; // already created
    final_screen = lv_obj_create(NULL);
    // black background
    lv_obj_set_style_bg_color(final_screen, KODE_BG_DARK, 0);
    lv_obj_set_style_bg_opa(final_screen, LV_OPA_100, 0);

    lv_obj_t * msg = lv_label_create(final_screen);
    lv_obj_add_style(msg, &style_title, 0);
    // Use the title color (KODE_TEXT_LIGHT)
    lv_label_set_text(msg, "YOU COMPLETED 5 CYCLES\nTAKE A WELL-DESERVED BREAK");
    lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
}

uint32_t getElapsedMillis() {
    if (currentState == STATE_PAUSED) {
        return pausedTime - sessionStartTime - totalPausedTime;
    } else {
        return millis() - sessionStartTime - totalPausedTime;
    }
}

uint32_t getElapsedSeconds() {
    return getElapsedMillis() / 1000;
}

uint32_t getRemainingSeconds() {
    uint32_t totalMillis = currentSessionMinutes * 60 * 1000;
    uint32_t elapsedMillis = getElapsedMillis();
    if (elapsedMillis >= totalMillis) {
        return 0;
    }
    return (totalMillis - elapsedMillis) / 1000;
}

bool isTimerComplete() {
    uint32_t totalMillis = currentSessionMinutes * 60 * 1000;
    uint32_t elapsedMillis = getElapsedMillis();
    return elapsedMillis >= totalMillis;
}

void updateTimer() {
    static uint32_t lastUpdate = 0;
    uint32_t now = millis();
    if (currentState == STATE_FINISHED) return; // freeze timer when finished
    
    if (now - lastUpdate < TIMER_UPDATE_MS) return;
    lastUpdate = now;
    
    if (currentState == STATE_WORK || currentState == STATE_BREAK || currentState == STATE_PAUSED) {
        uint32_t remaining = getRemainingSeconds();
        uint32_t minutes = remaining / 60;
        uint32_t seconds = remaining % 60;
        
        // Update timer display
        if (timer_display_label) {
            lv_label_set_text_fmt(timer_display_label, "%d:%02d", minutes, seconds);
        }
        
        // Update cycles display
        if (timer_cycles_label) {
            lv_label_set_text_fmt(timer_cycles_label, "%d CYCLES", completedCycles);
        }
        
        // Update progress dots (20 dots in arc)
        uint32_t totalSeconds = currentSessionMinutes * 60;
        uint32_t elapsed = getElapsedSeconds();
        float progress = (float)elapsed / totalSeconds;
        int activeDots = (int)(progress * 20);  // Changed from 24 to 20
        
        for (int i = 0; i < 20; i++) {  // Changed from 24 to 20
            if (i < activeDots) {
                // Use the warm orange color for filled dots
                lv_obj_set_style_bg_color(progress_dots[i], KODE_PROGRESS_FILL, 0);
                lv_obj_set_style_bg_opa(progress_dots[i], LV_OPA_100, 0);
            } else {
                // Use the light green color for empty dots
                lv_obj_set_style_bg_color(progress_dots[i], KODE_PROGRESS_EMPTY, 0);
                lv_obj_set_style_bg_opa(progress_dots[i], LV_OPA_100, 0);
            }
        }
        
        // Update cycle indicator dots (5 dots)
        lv_obj_t * cycle_dots_container = lv_obj_get_child(timer_screen, -3); // Get cycles container
        if (cycle_dots_container) {
            for (int i = 0; i < 5; i++) {
                lv_obj_t * cycle_dot = lv_obj_get_child(cycle_dots_container, i);
                if (cycle_dot) {
                    if (i < completedCycles) {
                        // Completed cycles use the warm orange color
                        lv_obj_set_style_bg_color(cycle_dot, KODE_PROGRESS_FILL, 0);
                        lv_obj_set_style_bg_opa(cycle_dot, LV_OPA_100, 0);
                    } else {
                        // Uncompleted cycles use the light green color
                        lv_obj_set_style_bg_color(cycle_dot, KODE_PROGRESS_EMPTY, 0);
                        lv_obj_set_style_bg_opa(cycle_dot, LV_OPA_100, 0);
                    }
                }
            }
        }
        
        // If timer reached exactly 0 and not already holding, trigger session completion
        if (isTimerComplete() && !completionHoldActive) {
            completionHoldActive = true;
            completionHoldStart = now; // use millis snapshot
            soundPlayed = false; // Reset for new completion
            Serial.println("Timer completed - starting completion hold");
        }
        // During hold show 00:00, play sound once, and wait ~5s before switching session
        if (completionHoldActive) {
            if (timer_display_label) lv_label_set_text_fmt(timer_display_label, "%d:%02d", 0, 0);
            // Play completion sound only once at start of hold
            if (!soundPlayed && soundEnabled) {
                soundPlayed = true;
                Serial.println("Playing completion sound");
                playCompletionSound(isWorkSession);
            }
            if (now - completionHoldStart >= 5000) {
                completionHoldActive = false;
                switchToNextSession();
            }
        }
    }
}

void updateNeoPixel() {
    static uint32_t lastUpdate = 0;
    static uint8_t lastR = 255, lastG = 255, lastB = 255; // Track last color to avoid unnecessary updates
    uint32_t now = millis();
    
    if (now - lastUpdate < LED_UPDATE_MS) return;
    lastUpdate = now;
    
    // Skip LED updates during completion hold to avoid instability
    if (completionHoldActive) {
        // Keep current color steady during completion
        return;
    }
    
    uint8_t newR = 0, newG = 0, newB = 0;
    bool needsUpdate = false;
    
    switch (currentState) {
        case STATE_CONFIG_WORK:
        case STATE_CONFIG_BREAK:
        case STATE_SOUND_CHOICE:
            // Orange blinking effect (stable)
            {
                bool blink = (now / 800) % 2; // Blink every 800ms
                if (blink) {
                    newR = 120;
                    newG = 60;
                    newB = 0;
                } else {
                    newR = 80;
                    newG = 40;
                    newB = 0;
                }
            }
            break;
            
        case STATE_WORK:
            // Blue progress indicator
            {
                uint32_t totalSeconds = currentSessionMinutes * 60;
                uint32_t elapsed = getElapsedSeconds();
                uint32_t remaining = totalSeconds - elapsed;
                
                if (remaining <= 30) {
                    // Last 30 seconds: blue blinking
                    bool blink = (now / 300) % 2; // Fast blink every 300ms
                    if (blink) {
                        newR = 0;
                        newG = 100;
                        newB = 255;
                    } else {
                        newR = 0;
                        newG = 50;
                        newB = 150;
                    }
                } else {
                    // Steady blue for all other times
                    newR = 0;
                    newG = 100;
                    newB = 200;
                }
            }
            break;
            
        case STATE_BREAK:
            // Green progress indicator
            {
                uint32_t totalSeconds = currentSessionMinutes * 60;
                uint32_t elapsed = getElapsedSeconds();
                uint32_t remaining = totalSeconds - elapsed;
                
                if (remaining <= 30) {
                    // Last 30 seconds: green blinking
                    bool blink = (now / 300) % 2; // Fast blink every 300ms
                    if (blink) {
                        newR = 50;
                        newG = 255;
                        newB = 0;
                    } else {
                        newR = 20;
                        newG = 150;
                        newB = 0;
                    }
                } else {
                    // Steady green for all other times
                    newR = 0;
                    newG = 150;
                    newB = 0;
                }
            }
            break;
            
        case STATE_PAUSED:
            // Yellow blinking
            {
                bool blink = (now / 1000) % 2;
                if (blink) {
                    newR = 150;
                    newG = 150;
                    newB = 0;
                } else {
                    newR = 0;
                    newG = 0;
                    newB = 0;
                }
            }
            break;
            
        default:
            newR = 0;
            newG = 0;
            newB = 0;
            break;
    }
    
    // Only update LED if color actually changed to reduce instability
    // Add minimum threshold to avoid micro-updates
    uint8_t colorDiff = abs(newR - lastR) + abs(newG - lastG) + abs(newB - lastB);
    if (colorDiff >= 3) { // Only update if total color change is significant
        lastR = newR;
        lastG = newG;
        lastB = newB;
        ledManager.setColor(newR, newG, newB);
        ledManager.show(); // Call show() only when we actually change the color
    }
}

// -----------------------------------------------------------------------------
// Sound playback using AudioManager
// -----------------------------------------------------------------------------

void playCompletionSound(bool /*isWorkComplete*/) {
    // Play a single pair: short beep, short gap, short beep.
    const uint32_t gapMs = 120;      // gap between the two beeps in a pair
    const int freq = 1600;           // tone frequency
    // Two short beeps (blocking). The completion hold will keep showing 00:00 for ~5s.
    audioManager.playBeep(freq, 70, 6000);
    delay(gapMs);
    audioManager.playBeep(freq, 70, 6000);
}
