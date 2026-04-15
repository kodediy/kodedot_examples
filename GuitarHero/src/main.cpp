/*
 * Guitar Hero Game for Kode Dot (ESP32-S3)
 * ------------------------------------------------------------
 * A rhythm game where notes fall from the top and you must press
 * the correct button when they reach the target zone.
 * 
 * Hardware:
 *  - 3 Buttons on GPIO 1, 13, and 40 (with internal pull-up)
 *  - Display for game visuals
 *  - LED for visual feedback
 */
#include <Arduino.h>
#include <lvgl.h>
#include <kodedot/display_manager.h>
#include <led_manager/LEDManager.h>
#include <kodedot/pin_config.h>
#include <TCA9555.h>
#include <audio_manager/AudioManager.h>
// #include <SD_MMC.h> // Disabled SD
#include <vector>

// Brand logo image (generated C array)
extern const lv_image_dsc_t logotipo;

// Brand fonts
extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30;
extern const lv_font_t Inter_40;
extern const lv_font_t Inter_50;

// Display manager instance
DisplayManager display;

// NeoPixel
static LEDManager led;

// IO Expander
static TCA9555 ioexp(IOEXP_I2C_ADDR);

// Audio Manager
static AudioManager audioManager;
static bool audio_ready = false;

// Game constants
#define BUTTON_PIN_1 1
#define BUTTON_PIN_2 13
#define BUTTON_PIN_3 41
#define NUM_LANES 3
#define MAX_NOTES 20 // Increased for safety
#define NOTE_SPEED 3          // pixels per frame (legacy)
#define TARGET_ZONE_Y 50      // Y position where notes should be hit (top of screen)
#define HIT_TOLERANCE 60      // pixels tolerance for hitting notes
#define SCREEN_HEIGHT 502
#define SCREEN_WIDTH 410
#define NOTE_SIZE 50
#define LANE_WIDTH (SCREEN_WIDTH / NUM_LANES)  // 136 pixels per lane
#define LANE_X_START 0

// Song constants
#define MS_PER_TICK 120
#define NOTE_SPEED_PX_MS 0.15f // Pixels per millisecond (Slower)
#define SONG_LEAD_IN_MS 2000  // Delay before first note to allow travel time
#define NOTE_TRAVEL_TIME_MS ((SCREEN_HEIGHT - TARGET_ZONE_Y) / NOTE_SPEED_PX_MS)

struct SongNote {
    uint32_t startTick;
    uint8_t lane;
    uint16_t frequency;
    uint8_t durationTicks;
    uint32_t hitTimeMs;
};

std::vector<SongNote> songNotes;
int nextNoteIndex = 0;

// Game state
enum GameState {
    STATE_MENU,
    STATE_PLAYING,
    STATE_GAME_OVER
};

struct Note {
    int lane;        // 0, 1, or 2
    float y;         // Y position (float for smooth movement)
    bool active;     // Is this note visible?
    lv_obj_t *obj;   // LVGL object for the note
    uint16_t frequency; // Frequency to play
    uint32_t hitTimeMs; // When it should hit target
};

struct GameData {
    GameState state;
    int score;
    int combo;
    int maxCombo;
    int notesHit;
    int notesMissed;
    Note notes[MAX_NOTES];
    uint32_t lastNoteSpawn;
    uint32_t gameStartTime;
    bool buttonPressed[NUM_LANES];
    bool buttonLastState[NUM_LANES];
};

static GameData game;

// UI elements
static lv_obj_t *score_label;
static lv_obj_t *combo_label;
static lv_obj_t *menu_screen;
static lv_obj_t *game_screen;
static lv_obj_t *gameover_screen;
static lv_obj_t *target_zone;
static lv_obj_t *lane_lines[NUM_LANES + 1];
static lv_obj_t *button_indicators[NUM_LANES];

// Game colors
static const lv_color_t COLOR_BG          = lv_color_hex(0x000000);
static const lv_color_t COLOR_LANE_1      = lv_color_hex(0x0000FF); // Blue
static const lv_color_t COLOR_LANE_2      = lv_color_hex(0xFFFFFF); // White
static const lv_color_t COLOR_LANE_3      = lv_color_hex(0xFF0000); // Red
static const lv_color_t COLOR_TARGET      = lv_color_hex(0xFFFFFF); // White
static const lv_color_t COLOR_TEXT        = lv_color_hex(0xFFFAF5);
static const lv_color_t COLOR_ACCENT      = lv_color_hex(0xFF7F1F);

// Forward declarations
void createMenuUI();
void createGameUI();
void createGameOverUI();
void startGame();
void updateGame();
void readButtons();
void spawnNote(const SongNote& songNote); // Modified prototype
void checkNoteHits();
void gameOver();
void resetGame();
void loadSong();

void setup() {
    Serial.begin(115200);
    Serial.println("Starting Guitar Hero Game...");

    // Initialize display
    if (!display.init()) {
        Serial.println("Error: Failed to initialize display");
        while(1) {
            delay(1000);
        }
    }

    // Initialize buttons with internal pull-up
    pinMode(BUTTON_PIN_1, INPUT_PULLUP);
    pinMode(BUTTON_PIN_2, INPUT_PULLUP);
    pinMode(BUTTON_PIN_3, INPUT_PULLUP);

    // Initialize IO Expander
    if (!ioexp.begin(INPUT)) {
        Serial.println("Warning: IO Expander not connected");
    }

    // Initialize Audio Manager
    AudioConfig audioCfg;
    audioCfg.spkSckPin = SPK_I2S_SCK;
    audioCfg.spkWsPin  = SPK_I2S_WS;
    audioCfg.spkDoutPin = SPK_I2S_DOUT;
    
    if (audioManager.init(audioCfg)) {
        audio_ready = true;
        audioManager.attachExpander(&ioexp, EXPANDER_SPK_SHUTDOWN);
        Serial.println("[Audio] initialized");
    } else {
        Serial.println("[Audio] init failed");
    }

    // Initialize LED manager
    LEDConfig ledCfg;
    ledCfg.pin = NEO_PIXEL_PIN;
    ledCfg.count = NEO_PIXEL_COUNT;
    ledCfg.brightness = 25; // ~10%
    
    if (!led.init(ledCfg)) {
        Serial.println("[LED] init failed");
    } else {
        Serial.println("[LED] initialized");
        led.turnOff();
        led.show();
    }

    // Load hardcoded song
    loadSong();

    // Create UI screens
    createMenuUI();
    createGameUI();
    createGameOverUI();
    
    // Start with menu
    game.state = STATE_MENU;
    lv_scr_load(menu_screen);
    
    Serial.println("System ready!");
}

void loop() {
    display.update();
    
    switch (game.state) {
        case STATE_MENU:
            // Check for any button press to start
            readButtons();
            if (game.buttonPressed[0] || game.buttonPressed[1] || game.buttonPressed[2]) {
                startGame();
            }
            break;
            
        case STATE_PLAYING:
            readButtons();
            updateGame();
            break;
            
        case STATE_GAME_OVER:
            // Check for button press to return to menu
            readButtons();
            if (game.buttonPressed[0] || game.buttonPressed[1] || game.buttonPressed[2]) {
                game.state = STATE_MENU;
                lv_scr_load(menu_screen);
                resetGame();
                // Ensure session is closed if not already (e.g. if gameOver wasn't called)
                if (audio_ready) audioManager.endToneSession();
            }
            break;
    }
    
    delay(5);
}

// ============================================================================
// GAME FUNCTIONS
// ============================================================================

void createMenuUI() {
    menu_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(menu_screen, COLOR_BG, 0);
    lv_obj_clear_flag(menu_screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Logo
    lv_obj_t * img_logo = lv_image_create(menu_screen);
    lv_image_set_src(img_logo, &logotipo);
    lv_obj_align(img_logo, LV_ALIGN_CENTER, 0, -40);
    
    // Title
    lv_obj_t * title = lv_label_create(menu_screen);
    lv_label_set_text(title, "GUITAR HERO");
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &Inter_40, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 20);
    
    // Instructions
    lv_obj_t * inst = lv_label_create(menu_screen);
    lv_label_set_text(inst, "Press any button to start");
    lv_obj_set_style_text_color(inst, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(inst, &Inter_20, 0);
    lv_obj_align(inst, LV_ALIGN_CENTER, 0, 60);
}

void createGameUI() {
    game_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(game_screen, COLOR_BG, 0);
    lv_obj_clear_flag(game_screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Score label (bottom center)
    score_label = lv_label_create(game_screen);
    lv_label_set_text(score_label, "Score: 0");
    lv_obj_set_style_text_color(score_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(score_label, &Inter_30, 0);
    lv_obj_align(score_label, LV_ALIGN_BOTTOM_MID, 0, -50);
    
    // Create lane dividers
    for (int i = 0; i <= NUM_LANES; i++) {
        lane_lines[i] = lv_obj_create(game_screen);
        lv_obj_set_size(lane_lines[i], 2, SCREEN_HEIGHT);
        lv_obj_set_pos(lane_lines[i], i * LANE_WIDTH, 0);
        lv_obj_set_style_bg_color(lane_lines[i], lv_color_hex(0x333333), 0);
        lv_obj_set_style_border_width(lane_lines[i], 0, 0);
    }
    
    // Create target zone (horizontal line where notes should be hit)
    target_zone = lv_obj_create(game_screen);
    lv_obj_set_size(target_zone, LANE_WIDTH * NUM_LANES, 4);
    lv_obj_set_pos(target_zone, 0, TARGET_ZONE_Y);
    lv_obj_set_style_bg_color(target_zone, COLOR_TARGET, 0);
    lv_obj_set_style_border_width(target_zone, 0, 0);
    
    // Create button indicators at top (where notes will reach)
    lv_color_t lane_colors[NUM_LANES] = {COLOR_LANE_1, COLOR_LANE_2, COLOR_LANE_3};
    for (int i = 0; i < NUM_LANES; i++) {
        button_indicators[i] = lv_obj_create(game_screen);
        lv_obj_set_size(button_indicators[i], LANE_WIDTH - 10, 80);
        lv_obj_set_pos(button_indicators[i], i * LANE_WIDTH + 5, 5);
        lv_obj_set_style_bg_color(button_indicators[i], lane_colors[i], 0);
        lv_obj_set_style_border_width(button_indicators[i], 2, 0);
        lv_obj_set_style_border_color(button_indicators[i], COLOR_TEXT, 0);
        lv_obj_set_style_opa(button_indicators[i], LV_OPA_30, 0);
    }
    
    // Initialize note objects (hidden)
    for (int i = 0; i < MAX_NOTES; i++) {
        game.notes[i].active = false;
        game.notes[i].obj = lv_obj_create(game_screen);
        lv_obj_set_size(game.notes[i].obj, NOTE_SIZE, NOTE_SIZE);
        lv_obj_set_style_border_width(game.notes[i].obj, 2, 0);
        lv_obj_set_style_border_color(game.notes[i].obj, COLOR_TEXT, 0);
        lv_obj_set_style_radius(game.notes[i].obj, 5, 0);
        lv_obj_add_flag(game.notes[i].obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void createGameOverUI() {
    gameover_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(gameover_screen, COLOR_BG, 0);
    lv_obj_clear_flag(gameover_screen, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_t * title = lv_label_create(gameover_screen);
    lv_label_set_text(title, "GAME OVER");
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &Inter_50, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);
    
    // We'll update these in gameOver()
    lv_obj_t * score = lv_label_create(gameover_screen);
    lv_label_set_text(score, "Score: 0");
    lv_obj_set_style_text_color(score, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(score, &Inter_30, 0);
    lv_obj_align(score, LV_ALIGN_CENTER, 0, -10);
    
    lv_obj_t * accuracy = lv_label_create(gameover_screen);
    lv_label_set_text(accuracy, "Accuracy: 0%");
    lv_obj_set_style_text_color(accuracy, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(accuracy, &Inter_20, 0);
    lv_obj_align(accuracy, LV_ALIGN_CENTER, 0, 25);
    
    lv_obj_t * combo = lv_label_create(gameover_screen);
    lv_label_set_text(combo, "Max Combo: 0");
    lv_obj_set_style_text_color(combo, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(combo, &Inter_20, 0);
    lv_obj_align(combo, LV_ALIGN_CENTER, 0, 50);
    
    lv_obj_t * restart = lv_label_create(gameover_screen);
    lv_label_set_text(restart, "Press any button to continue");
    lv_obj_set_style_text_color(restart, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(restart, &Inter_20, 0);
    lv_obj_align(restart, LV_ALIGN_BOTTOM_MID, 0, -20);
}

void resetGame() {
    game.score = 0;
    game.combo = 0;
    game.maxCombo = 0;
    game.notesHit = 0;
    game.notesMissed = 0;
    game.lastNoteSpawn = 0;
    nextNoteIndex = 0; // Reset song index
    
    for (int i = 0; i < MAX_NOTES; i++) {
        game.notes[i].active = false;
        if (game.notes[i].obj) {
            lv_obj_add_flag(game.notes[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }
    
    for (int i = 0; i < NUM_LANES; i++) {
        game.buttonPressed[i] = false;
        game.buttonLastState[i] = false;
    }
}

void startGame() {
    Serial.println("[Game] Starting sequence initiated...");
    
    // 1. Reset Logic
    Serial.println("[Game] Resetting state...");
    resetGame();
    delay(50);

    // 2. UI Switch
    Serial.println("[Game] Switching UI...");
    if(game_screen) {
        lv_scr_load(game_screen);
        // Force a task handler run to ensure UI updates before heavy processing
        lv_timer_handler(); 
        delay(50);
    } else {
        Serial.println("[Game] Error: Game screen not initialized");
        return;
    }
    
    // 3. LED Feedback
    Serial.println("[Game] LED Feedback...");
    led.setPixelColor(0, 0, 255, 0); // Green
    led.show();
    delay(100);
    
    // 4. Audio Init
    if (audio_ready) {
        Serial.println("[Game] Starting Audio Session...");
        if(!audioManager.beginToneSession()) {
            Serial.println("[Game] Warning: Audio session failed to start");
        } else {
            audioManager.playToneChunk(1000, 200, 1000);
        }
    }
    delay(100);

    // 5. Finalize
    led.turnOff();
    led.show();
    
    game.state = STATE_PLAYING;
    game.gameStartTime = millis();
    Serial.printf("[Game] Started at %lu ms. Song notes: %d\n", game.gameStartTime, (int)songNotes.size());
}

void loadSong() {
    songNotes.clear();
    Serial.println("Loading hardcoded Mario theme...");

    // Helper lambda to add notes
    auto addNote = [](uint32_t tick, uint8_t lane, uint16_t freq) {
        SongNote note;
        note.startTick = tick;
        note.lane = lane;
        note.frequency = freq;
        note.durationTicks = 2;
        note.hitTimeMs = (tick * MS_PER_TICK) + SONG_LEAD_IN_MS;
        songNotes.push_back(note);
    };

    // Frequencies
    const uint16_t E5 = 659;
    const uint16_t C5 = 523;
    const uint16_t G5 = 784;
    const uint16_t G4 = 392;
    const uint16_t E4 = 330;
    const uint16_t A4 = 440;
    const uint16_t B4 = 494;
    const uint16_t Bb4 = 466;

    // --- Intro ---
    addNote(0,  2, E5);
    addNote(2,  2, E5);
    addNote(4,  2, E5);
    addNote(6,  1, C5);
    addNote(8,  2, E5);
    addNote(12, 2, G5);
    addNote(20, 0, G4);

    // --- Theme A ---
    addNote(28, 1, C5);
    addNote(32, 0, G4);
    addNote(36, 0, E4);
    
    addNote(40, 1, A4);
    addNote(44, 2, B4);
    addNote(48, 2, Bb4);
    addNote(50, 1, A4);

    addNote(54, 0, G4);
    addNote(56, 2, E5);
    addNote(58, 2, G5);
    addNote(60, 2, A4 * 2); // A5
    
    addNote(64, 2, 698); // F5
    addNote(66, 2, G5);
    
    addNote(70, 2, E5);
    addNote(72, 1, C5);
    addNote(74, 1, 587); // D5
    addNote(76, 2, B4);

    // Repeat Theme A slightly varied
    addNote(84, 1, C5);
    addNote(88, 0, G4);
    addNote(92, 0, E4);
    
    addNote(96, 1, A4);
    addNote(100, 2, B4);
    addNote(104, 2, Bb4);
    addNote(106, 1, A4);

    addNote(110, 0, G4);
    addNote(112, 2, E5);
    addNote(114, 2, G5);
    addNote(116, 2, A4 * 2); // A5
    
    addNote(120, 2, 698); // F5
    addNote(122, 2, G5);
    
    addNote(126, 2, E5);
    addNote(128, 1, C5);
    addNote(130, 1, 587); // D5
    addNote(132, 2, B4);

    Serial.printf("Loaded %d notes\n", songNotes.size());
}

void readButtons() {
    // Read buttons (active LOW with pull-up)
    bool current[NUM_LANES];
    current[0] = (digitalRead(BUTTON_PIN_1) == LOW);
    current[1] = (digitalRead(BUTTON_PIN_2) == LOW);
    current[2] = (digitalRead(BUTTON_PIN_3) == LOW);
    
    for (int i = 0; i < NUM_LANES; i++) {
        // Detect button press (transition from not pressed to pressed)
        game.buttonPressed[i] = current[i] && !game.buttonLastState[i];
        game.buttonLastState[i] = current[i];
        
        // Update button indicator opacity
        if (current[i]) {
            lv_obj_set_style_opa(button_indicators[i], LV_OPA_100, 0);
        } else {
            lv_obj_set_style_opa(button_indicators[i], LV_OPA_30, 0);
        }
    }
}

void spawnNote(const SongNote& songNote) {
    // Find an inactive note slot
    for (int i = 0; i < MAX_NOTES; i++) {
        if (!game.notes[i].active) {
            if (game.notes[i].obj == NULL) {
                Serial.printf("[Game] Error: Note object %d is NULL, skipping\n", i);
                continue;
            }

            game.notes[i].active = true;
            game.notes[i].lane = songNote.lane;
            game.notes[i].hitTimeMs = songNote.hitTimeMs;
            game.notes[i].frequency = songNote.frequency;
            
            // Set initial position (top of screen)
            lv_obj_set_pos(game.notes[i].obj, LANE_X_START + (songNote.lane * LANE_WIDTH) + (LANE_WIDTH - NOTE_SIZE) / 2, 0);
            
            // Set color based on lane
            lv_color_t color;
            switch(songNote.lane) {
                case 0: color = COLOR_LANE_1; break; // Blue
                case 1: color = COLOR_LANE_2; break; // White
                case 2: color = COLOR_LANE_3; break; // Red
                default: color = lv_palette_main(LV_PALETTE_GREY); break;
            }
            lv_obj_set_style_bg_color(game.notes[i].obj, color, 0);
            
            lv_obj_clear_flag(game.notes[i].obj, LV_OBJ_FLAG_HIDDEN);
            break;
        }
    }
}

void checkNoteHits() {
    for (int i = 0; i < NUM_LANES; i++) {
        if (game.buttonPressed[i]) {
            // Check if there's a note in this lane within hit tolerance
            bool hit = false;
            for (int j = 0; j < MAX_NOTES; j++) {
                if (game.notes[j].active && game.notes[j].lane == i) {
                    int distance = abs(game.notes[j].y - TARGET_ZONE_Y);
                    if (distance < HIT_TOLERANCE) {
                        // Hit!
                        game.notes[j].active = false;
                        lv_obj_add_flag(game.notes[j].obj, LV_OBJ_FLAG_HIDDEN);
                        
                        game.score += 100 + (game.combo * 10);
                        game.combo++;
                        game.notesHit++;
                        
                        if (game.combo > game.maxCombo) {
                            game.maxCombo = game.combo;
                        }
                        
                        // LED feedback - green for hit
                        led.setPixelColor(0, 0, 255, 0);
                        led.show();

                        // Audio feedback
                        if (audio_ready) {
                            // Use note frequency
                            Serial.printf("Hit! Playing freq: %d\n", game.notes[j].frequency);
                            audioManager.playToneChunk(game.notes[j].frequency, 100, 1000);
                        }
                        
                        hit = true;
                        break;
                    }
                }
            }
            
            if (!hit) {
                // Missed hit (pressed but no note nearby)
                game.combo = 0;
                led.setPixelColor(0, 255, 0, 0); // Red for miss
                led.show();

                // Audio feedback
                if (audio_ready) {
                    Serial.println("Miss! Playing error tone");
                    audioManager.playToneChunk(150, 100, 1000); // Low buzz for miss (low volume)
                }
            }
        }
    }
}

void updateGame() {
    uint32_t now = millis();
    uint32_t gameTime = now - game.gameStartTime;
    
    // Calculate travel time from bottom to target
    // distance = SCREEN_HEIGHT - TARGET_ZONE_Y
    // time = distance / speed
    float travelDist = SCREEN_HEIGHT - TARGET_ZONE_Y;
    uint32_t travelTime = travelDist / NOTE_SPEED_PX_MS;
    
    // Spawn notes
    if (!songNotes.empty()) {
        while (nextNoteIndex < songNotes.size()) {
            const auto& note = songNotes[nextNoteIndex];
            // Spawn note if it's time (hit time - travel time)
            // Use int32_t for safe comparison with potentially negative spawn times
            int32_t spawnTime = (int32_t)note.hitTimeMs - NOTE_TRAVEL_TIME_MS;
            
            if ((int32_t)gameTime >= spawnTime) {
                spawnNote(note);
                nextNoteIndex++;
            } else {
                break; // Notes are sorted by time, so we can stop checking
            }
        }
    }
    
    // Update note positions
    for (int i = 0; i < MAX_NOTES; i++) {
        if (game.notes[i].active) {
            // Position based on time:
            // y = TARGET_ZONE_Y + (hitTime - gameTime) * speed
            // If gameTime == hitTime, y = TARGET_ZONE_Y
            
            int32_t timeDiff = game.notes[i].hitTimeMs - gameTime;
            game.notes[i].y = TARGET_ZONE_Y + timeDiff * NOTE_SPEED_PX_MS;
            
            lv_obj_set_pos(game.notes[i].obj, 
                          game.notes[i].lane * LANE_WIDTH + (LANE_WIDTH - NOTE_SIZE) / 2,
                          (int)game.notes[i].y);
            
            // Check if note went past the target zone (missed)
            // Allow some tolerance past the target
            if (game.notes[i].y < TARGET_ZONE_Y - HIT_TOLERANCE - 20) {
                game.notes[i].active = false;
                lv_obj_add_flag(game.notes[i].obj, LV_OBJ_FLAG_HIDDEN);
                game.notesMissed++;
                game.combo = 0;
                
                // LED feedback - red for miss
                led.setPixelColor(0, 255, 0, 0);
                led.show();
                
                // Audio feedback for missing a note (letting it pass)
                if (audio_ready) {
                     // Play a short low tone to indicate miss
                     audioManager.playToneChunk(100, 100, 1000);
                }
            }
        }
    }
    
    // Check for note hits
    checkNoteHits();
    
    // Update UI
    lv_label_set_text_fmt(score_label, "Score: %d", game.score);
    
    // Turn off LED after brief flash
    static uint32_t ledTimer = 0;
    if (now - ledTimer > 100) {
        led.turnOff();
        led.show();
        ledTimer = now;
    }
    
    // Check for game over (when all notes are done and off screen)
    if (nextNoteIndex >= songNotes.size()) {
        bool anyActive = false;
        for(int i=0; i<MAX_NOTES; i++) {
            if(game.notes[i].active) anyActive = true;
        }
        if(!anyActive) {
             // Wait a bit before game over
             static uint32_t endTimer = 0;
             if(endTimer == 0) endTimer = now;
             if(now - endTimer > 2000) {
                 gameOver();
                 endTimer = 0;
             }
        }
    }
}

void gameOver() {
    game.state = STATE_GAME_OVER;
    
    // Update game over screen with final stats
    lv_obj_t * score_obj = lv_obj_get_child(gameover_screen, -4);
    lv_label_set_text_fmt(score_obj, "Score: %d", game.score);
    
    int total = game.notesHit + game.notesMissed;
    int accuracy = total > 0 ? (game.notesHit * 100) / total : 0;
    
    lv_obj_t * accuracy_obj = lv_obj_get_child(gameover_screen, -3);
    lv_label_set_text_fmt(accuracy_obj, "Accuracy: %d%%", accuracy);
    
    lv_obj_t * combo_obj = lv_obj_get_child(gameover_screen, -2);
    lv_label_set_text_fmt(combo_obj, "Max Combo: %d", game.maxCombo);
    
    lv_scr_load(gameover_screen);
    
    // LED visual feedback
    led.setPixelColor(0, 255, 0, 255); // Magenta for game over
    led.show();
    
    if (audio_ready) {
        audioManager.playToneChunk(200, 500, 1000); // Long low buzz (low volume)
        audioManager.endToneSession(); // End persistent session
    }
    
    delay(500);
    led.turnOff();
    led.show();
}