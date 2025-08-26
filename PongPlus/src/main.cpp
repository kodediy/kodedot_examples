/*
 * Pong Game for Kode Dot (ESP32-S3)
 * ------------------------------------------------------------
 * A retro vertical Pong game with realistic physics, pad controls, and
 * a classic retro aesthetic.
 * 
 * Features:
 *  - Start menu with play button
 *  - 3 lives system
 *  - Vertical gameplay (ball bounces off top wall)
 *  - Realistic ball physics with inertia
 *  - Pad controls for paddle movement
 *  - TOP button for pause/resume
 *  - Retro-style graphics and colors
 *  - Score tracking
 */

#include <Arduino.h>
#include <lvgl.h>
#include <kodedot/display_manager.h>
#include <TCA9555.h>
#include <kodedot/pin_config.h>
#include <Adafruit_NeoPixel.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

// Brand fonts (Inter)
extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30;
extern const lv_font_t Inter_40;
extern const lv_font_t Inter_50;
extern const lv_font_t Inter_70;
extern const lv_font_t Inter_100;
extern const lv_font_t Inter_150;
extern const lv_font_t Inter_200;

// Display manager instance
DisplayManager display;

// IO Expander
static TCA9555 ioexp(IOEXP_I2C_ADDR);

// NeoPixel
static Adafruit_NeoPixel pixel(NEO_PIXEL_COUNT, NEO_PIXEL_PIN, LED_STRIP_COLOR_ORDER + LED_STRIP_TIMING);

// Game state
enum GameState {
    MENU,
    PLAYING,
    PAUSED,
    MULTIPLAYER_MENU,
    HOST_WAITING,
    CLIENT_SEARCHING,
    MULTIPLAYER_PLAYING
};

static GameState gameState = MENU;
static bool gameInitialized = false;

// Game objects
struct Paddle {
    float x, y;
    float width, height;
    float speed;
};

struct Ball {
    float x, y;
    float vx, vy;
    float radius;
    float speed;
};

struct Game {
    Paddle paddle;
    Ball ball;
    int score;
    float gameSpeed;
    bool ballLaunched;
    uint32_t lastUpdate;
    float difficulty;
    uint32_t lastScoreTime;
    int bounceCount; // Contador de rebotes para incremento progresivo
    float currentBallSpeed; // Velocidad actual de la pelota
};

static Game game;

// ESP-NOW Multiplayer structures
// Nota: Usamos __attribute__((packed)) para controlar el tamaño exacto.
// Campos añadidos: currentBallSpeed para sincronizar la velocidad progresiva.
struct ESPNowMessage {
    uint8_t type;            // 1 byte
    float ballX;             // 4 -> 5
    float ballY;             // 4 -> 9
    float ballVX;            // 4 -> 13
    float ballVY;            // 4 -> 17
    float currentBallSpeed;  // 4 -> 21 (para sincronizar incrementos)
    float paddleX;           // 4 -> 25
    int   score;             // 4 -> 29
    bool  ballActive;        // 1 -> 30
    char  roomID[8];         // 8 -> 38
    char  deviceID[8];       // 8 -> 46
    uint8_t padding[18];     // 18 -> 64 bytes totales
} __attribute__((packed));
static_assert(sizeof(ESPNowMessage) == 64, "ESPNowMessage debe medir 64 bytes");

// ESP-NOW variables
static uint8_t peerMAC[6];
static bool isConnected = false;
static bool isHost = false;
static String deviceID;
static String roomID;
static uint32_t lastHeartbeat = 0;
static bool ballOnMyScreen = true;
static std::vector<String> availableRooms;

// Debug flags (poner a 1 según necesidad)
static const bool DEBUG_DRAW_BALL = false;      // Logs continuos de draw
static const bool DEBUG_BALL_TRANSFER = true;   // Logs de transferencia
static const bool DEBUG_RAW_BYTES = false;      // Hex dump de cada mensaje

// Message types
#define MSG_ROOM_ANNOUNCE 1
#define MSG_ROOM_SEARCH 2
#define MSG_JOIN_REQUEST 3
#define MSG_JOIN_RESPONSE 4
#define MSG_GAME_UPDATE 5
#define MSG_BALL_TRANSFER 6
#define MSG_HEARTBEAT 7

// UI objects
static lv_obj_t *gameScreen;
static lv_obj_t *menuScreen;
static lv_obj_t *multiplayerMenuScreen;
static lv_obj_t *hostWaitingScreen;
static lv_obj_t *clientSearchingScreen;
static lv_obj_t *scoreLabel;
static lv_obj_t *playButton;
static lv_obj_t *multiplayerButton;
static lv_obj_t *hostButton;
static lv_obj_t *joinButton;
static lv_obj_t *pauseOverlay;
static lv_obj_t *waitingLabel;
static lv_obj_t *roomList;
static lv_obj_t *hostRoomIdLabel; // label grande con el ID de la sala

// Game constants
static const float PADDLE_WIDTH = 80.0f;
static const float PADDLE_HEIGHT = 15.0f;
static const float BALL_RADIUS = 8.0f;
static const float PADDLE_SPEED = 280.0f; // Reducido para mejor control
static const float INITIAL_BALL_SPEED = 220.0f; // Velocidad base mucho más rápida
static const float MAX_BALL_SPEED = 600.0f; // Velocidad máxima aumentada
static const float FRICTION = 0.98f;
static const float PADDLE_BOUNCE = 1.2f;
static const float WALL_BOUNCE = 0.95f;
static const float SPEED_INCREMENT = 12.0f; // Incremento de velocidad mayor por rebote
static const float DIFFICULTY_THRESHOLD = 5; // Rebotes necesarios para incrementar dificultad

// Screen dimensions (runtime-detected)
static int SCREEN_WIDTH = 0;
static int SCREEN_HEIGHT = 0;

// Logical rounded corner radius (runtime, proportion of min dimension). Field se dibuja rectangular.
static float CORNER_RADIUS = 0.0f; // assigned after display init
static lv_obj_t *fieldObj = nullptr;

// Retro color palette
static const lv_color_t COLOR_BG = lv_color_hex(0x000000);         // Black
static const lv_color_t COLOR_PADDLE = lv_color_hex(0xFFFFFF);     // White
static const lv_color_t COLOR_BALL = lv_color_hex(0xFFFFFF);       // White
static const lv_color_t COLOR_TEXT = lv_color_hex(0xFFFFFF);       // White
static const lv_color_t COLOR_ACCENT = lv_color_hex(0xFFFFFF);     // White
static const lv_color_t COLOR_LINE = lv_color_hex(0x000000);       // Black (invisible border)
static const lv_color_t COLOR_SCORE = lv_color_hex(0xFFFFFF);      // White

// Styles
static lv_style_t style_bg;
static lv_style_t style_text;
static lv_style_t style_title;
static lv_style_t style_button;
static lv_style_t style_button_pressed;
static lv_style_t style_score;

// Forward declarations
void createStyles();
void createMenuScreen();
void createMultiplayerMenuScreen();
void createHostWaitingScreen();
void createClientSearchingScreen();
void createGameScreen();
void initGame();
void updateGame();
void updateMultiplayerGame();
void drawGame();
void handleInput();
void resetBall();
void checkCollisions();
void checkMultiplayerCollisions();
void updatePaddle(Paddle &paddle);
void resetGame();

// ESP-NOW functions
void initESPNow();
void onDataReceive(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len);
void onDataSent(const uint8_t *mac, esp_now_send_status_t status);
void sendGameUpdate();
void sendBallTransfer();
void startAsHost();
void startAsClient();
void searchForRooms();
void announceRoom();
void joinRoom(String roomId);
String generateDeviceID();
String generateRoomID();

// Objetos globales para paddle y bola
static lv_obj_t *paddleObj = nullptr;
static lv_obj_t *ballObj = nullptr;

void setup() {
    Serial.begin(115200);
    Serial.println("Starting Vertical Pong Game...");

    // Initialize display
    if (!display.init()) {
        Serial.println("Error: Failed to initialize display");
        while(1) delay(1000);
    }

    // Initialize IO Expander
    if (!ioexp.begin(INPUT)) {
        Serial.println("Warning: IO Expander not connected");
    }

    // Configure TOP button
  pinMode(BUTTON_TOP, INPUT_PULLUP);

    // Initialize NeoPixel
  pixel.begin();
  pixel.setBrightness(51);
  pixel.clear();
  pixel.show();

    // Detect actual display resolution (LVGL default display)
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
        SCREEN_WIDTH = lv_disp_get_hor_res(disp);
        SCREEN_HEIGHT = lv_disp_get_ver_res(disp);
    } else {
        // Fallback to known defaults
        SCREEN_WIDTH = 410;
        SCREEN_HEIGHT = 502;
    }
    CORNER_RADIUS = (float) std::min(SCREEN_WIDTH, SCREEN_HEIGHT) * 0.18f; // 18% tamaño esquinas lógicas

    // Initialize ESP-NOW
    initESPNow();

    // Create UI
    createStyles();
    createMenuScreen();
    createMultiplayerMenuScreen();
    createHostWaitingScreen();
    createClientSearchingScreen();
    createGameScreen();
    
    // Show menu first
    lv_scr_load(menuScreen);
    
    // Initialize game state
    game.score = 0;
    
    Serial.printf("Vertical Pong Game ready! (%dx%d, logical corner R=%.1f)\n", SCREEN_WIDTH, SCREEN_HEIGHT, CORNER_RADIUS);
}

void loop() {
    // Update display
    display.update();
    
    // Handle input
    handleInput();
    
    // Update game logic
    if (gameState == PLAYING) {
        updateGame();
        drawGame();
    } else if (gameState == MULTIPLAYER_PLAYING) {
        updateMultiplayerGame();
        drawGame();
    } else if (gameState == HOST_WAITING) {
        // Host waiting for clients
        static uint32_t lastAnnounce = 0;
        uint32_t now = millis();
        
        // Re-announce room every 3 seconds
        if (now - lastAnnounce > 3000) {
            announceRoom();
            lastAnnounce = now;
            Serial.printf("Host waiting... Room: %s (connected: %s)\n", roomID.c_str(), isConnected ? "YES" : "NO");
        }
        
        if (isConnected) {
            Serial.println("=== CLIENT JOINED - STARTING GAME ===");
            gameState = MULTIPLAYER_PLAYING;
            if (!gameInitialized) {
                initGame();
                gameInitialized = true;
            } else {
                resetGame();
            }
            lv_scr_load(gameScreen);
            drawGame();
            Serial.printf("Game started - I am HOST, ball on my screen: %s\n", ballOnMyScreen ? "YES" : "NO");
        }
    } else if (gameState == CLIENT_SEARCHING) {
        // Client searching for rooms
        static uint32_t lastSearch = 0;
        uint32_t now = millis();
        
        // Re-search every 3 seconds
        if (now - lastSearch > 3000) {
            searchForRooms();
            lastSearch = now;
            Serial.printf("Client searching... Found %d rooms (connected: %s)\n", availableRooms.size(), isConnected ? "YES" : "NO");
        }
        
        if (isConnected) {
            Serial.println("=== JOINED ROOM - STARTING GAME ===");
            gameState = MULTIPLAYER_PLAYING;
            if (!gameInitialized) {
                initGame();
                gameInitialized = true;
            } else {
                resetGame();
            }
            lv_scr_load(gameScreen);
            drawGame();
            Serial.printf("Game started - I am CLIENT, ball on my screen: %s\n", ballOnMyScreen ? "YES" : "NO");
        }
    }
    
    delay(8); // ~120 FPS
}

void createStyles() {
    // Background style
    lv_style_init(&style_bg);
    lv_style_set_bg_color(&style_bg, COLOR_BG);
    
    // Text style
    lv_style_init(&style_text);
    lv_style_set_text_color(&style_text, COLOR_TEXT);
    lv_style_set_text_font(&style_text, &Inter_30);
    
    // Title style
    lv_style_init(&style_title);
    lv_style_set_text_color(&style_title, COLOR_ACCENT);
    lv_style_set_text_font(&style_title, &Inter_100);
    
    // Button style
    lv_style_init(&style_button);
    lv_style_set_bg_color(&style_button, COLOR_ACCENT);
    lv_style_set_text_color(&style_button, COLOR_BG);
    lv_style_set_text_font(&style_button, &Inter_40);
    lv_style_set_radius(&style_button, 10);
    lv_style_set_pad_all(&style_button, 20);
    
    // Button pressed style
    lv_style_init(&style_button_pressed);
    lv_style_set_bg_color(&style_button_pressed, COLOR_TEXT);
    lv_style_set_text_color(&style_button_pressed, COLOR_BG);
    
    // Score style
    lv_style_init(&style_score);
    lv_style_set_text_color(&style_score, COLOR_SCORE);
    lv_style_set_text_font(&style_score, &Inter_40);
}

void createMenuScreen() {
    menuScreen = lv_obj_create(NULL);
    lv_obj_add_style(menuScreen, &style_bg, 0);
    
    // Title (más arriba y sin subtítulo extra)
    lv_obj_t *title = lv_label_create(menuScreen);
    lv_obj_add_style(title, &style_title, 0);
    lv_label_set_text(title, "PONG");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);
    
    // Play button (single player)
    playButton = lv_btn_create(menuScreen);
    lv_obj_add_style(playButton, &style_button, 0);
    lv_obj_add_style(playButton, &style_button_pressed, LV_STATE_PRESSED);
    
    lv_obj_t *playLabel = lv_label_create(playButton);
    lv_label_set_text(playLabel, "LOCAL");
    lv_obj_center(playLabel);
    
    // Bajamos ambos botones y aumentamos separación
    lv_obj_align(playButton, LV_ALIGN_CENTER, 0, -20);
    
    // Multiplayer button
    multiplayerButton = lv_btn_create(menuScreen);
    lv_obj_add_style(multiplayerButton, &style_button, 0);
    lv_obj_add_style(multiplayerButton, &style_button_pressed, LV_STATE_PRESSED);
    
    lv_obj_t *multiplayerLabel = lv_label_create(multiplayerButton);
    lv_label_set_text(multiplayerLabel, "ONLINE");
    lv_obj_center(multiplayerLabel);
    
    lv_obj_align(multiplayerButton, LV_ALIGN_CENTER, 0, 80); // más arriba y más separado
    
    // Play button event
    lv_obj_add_event_cb(playButton, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            gameState = PLAYING;
            if (!gameInitialized) {
                initGame();
                gameInitialized = true;
            } else {
                resetGame();
            }
            lv_scr_load(gameScreen);
            // Draw initial game state
            drawGame();
        }
    }, LV_EVENT_CLICKED, NULL);
    
    // Multiplayer button event
    lv_obj_add_event_cb(multiplayerButton, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            lv_scr_load(multiplayerMenuScreen);
        }
    }, LV_EVENT_CLICKED, NULL);
}

void createMultiplayerMenuScreen() {
    multiplayerMenuScreen = lv_obj_create(NULL);
    lv_obj_add_style(multiplayerMenuScreen, &style_bg, 0);
    // Se elimina texto de título para pantalla más limpia
    
    // Host button
    hostButton = lv_btn_create(multiplayerMenuScreen);
    lv_obj_add_style(hostButton, &style_button, 0);
    lv_obj_add_style(hostButton, &style_button_pressed, LV_STATE_PRESSED);
    
    lv_obj_t *hostLabel = lv_label_create(hostButton);
    lv_label_set_text(hostLabel, "HOST ROOM");
    lv_obj_center(hostLabel);
    
    lv_obj_align(hostButton, LV_ALIGN_CENTER, 0, -50);
    
    // Join button
    joinButton = lv_btn_create(multiplayerMenuScreen);
    lv_obj_add_style(joinButton, &style_button, 0);
    lv_obj_add_style(joinButton, &style_button_pressed, LV_STATE_PRESSED);
    
    lv_obj_t *joinLabel = lv_label_create(joinButton);
    lv_label_set_text(joinLabel, "JOIN ROOM");
    lv_obj_center(joinLabel);
    
    lv_obj_align(joinButton, LV_ALIGN_CENTER, 0, 50);
    
    // Back button
    lv_obj_t *backButton = lv_btn_create(multiplayerMenuScreen);
    lv_obj_add_style(backButton, &style_button, 0);
    lv_obj_add_style(backButton, &style_button_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(backButton, 160, 60);
    
    lv_obj_t *backLabel = lv_label_create(backButton);
    lv_label_set_text(backLabel, "BACK");
    lv_obj_center(backLabel);
    lv_obj_set_style_text_font(backLabel, &Inter_40, 0);
    
    // Back centrado abajo
    lv_obj_align(backButton, LV_ALIGN_BOTTOM_MID, 0, -30);
    
    // Host button event
    lv_obj_add_event_cb(hostButton, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            startAsHost();
        }
    }, LV_EVENT_CLICKED, NULL);
    
    // Join button event
    lv_obj_add_event_cb(joinButton, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            startAsClient();
        }
    }, LV_EVENT_CLICKED, NULL);
    
    // Back button event
    lv_obj_add_event_cb(backButton, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            lv_scr_load(menuScreen);
        }
    }, LV_EVENT_CLICKED, NULL);
}

void createHostWaitingScreen() {
    hostWaitingScreen = lv_obj_create(NULL);
    lv_obj_add_style(hostWaitingScreen, &style_bg, 0);
    
    // Mostrar solo el Room ID grande centrado (sin títulos extra)
    hostRoomIdLabel = lv_label_create(hostWaitingScreen);
    lv_obj_add_style(hostRoomIdLabel, &style_title, 0);
    lv_label_set_text_fmt(hostRoomIdLabel, "%s", roomID.c_str());
    lv_obj_align(hostRoomIdLabel, LV_ALIGN_CENTER, 0, -10);
    
    // Mensaje de espera debajo
    waitingLabel = lv_label_create(hostWaitingScreen);
    lv_obj_add_style(waitingLabel, &style_text, 0);
    lv_label_set_text(waitingLabel, "Waiting player...");
    lv_obj_align(waitingLabel, LV_ALIGN_CENTER, 0, 60);
    
    // Back button
    lv_obj_t *backButton = lv_btn_create(hostWaitingScreen);
    lv_obj_add_style(backButton, &style_button, 0);
    lv_obj_add_style(backButton, &style_button_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(backButton, 160, 60);
    
    lv_obj_t *backLabel = lv_label_create(backButton);
    lv_label_set_text(backLabel, "BACK");
    lv_obj_center(backLabel);
    lv_obj_set_style_text_font(backLabel, &Inter_40, 0);
    
    lv_obj_align(backButton, LV_ALIGN_BOTTOM_MID, 0, -40);
    
    lv_obj_add_event_cb(backButton, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            gameState = MENU;
            isConnected = false;
            lv_scr_load(menuScreen);
        }
    }, LV_EVENT_CLICKED, NULL);
}

void createClientSearchingScreen() {
    clientSearchingScreen = lv_obj_create(NULL);
    lv_obj_add_style(clientSearchingScreen, &style_bg, 0);
    
    // Sin título JOIN ROOM
    
    // Search status
    lv_obj_t *searchLabel = lv_label_create(clientSearchingScreen);
    lv_obj_add_style(searchLabel, &style_text, 0);
    lv_label_set_text(searchLabel, "Searching rooms...");
    lv_obj_align(searchLabel, LV_ALIGN_TOP_MID, 0, 30);
    
    // Room list (will be populated dynamically)
    roomList = lv_list_create(clientSearchingScreen);
    lv_obj_set_size(roomList, 340, 240);
    // Colocar más arriba entre texto y botón back
    lv_obj_align(roomList, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_style(roomList, &style_bg, 0);
    // Ajustar padding para que elementos empiecen más centrados
    lv_obj_set_style_pad_top(roomList, 60, 0);
    
    // Back button
    lv_obj_t *backButton = lv_btn_create(clientSearchingScreen);
    lv_obj_add_style(backButton, &style_button, 0);
    lv_obj_add_style(backButton, &style_button_pressed, LV_STATE_PRESSED);
    lv_obj_set_size(backButton, 160, 60);
    
    lv_obj_t *backLabel = lv_label_create(backButton);
    lv_label_set_text(backLabel, "BACK");
    lv_obj_center(backLabel);
    lv_obj_set_style_text_font(backLabel, &Inter_40, 0);
    
    lv_obj_align(backButton, LV_ALIGN_BOTTOM_MID, 0, -30);
    
    lv_obj_add_event_cb(backButton, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            gameState = MENU;
            availableRooms.clear();
            lv_scr_load(menuScreen);
        }
    }, LV_EVENT_CLICKED, NULL);
}

void createWaitingScreen() {
    // Esta función se mantiene para compatibilidad, pero ya no se usa
}

void createGameScreen() {
    gameScreen = lv_obj_create(NULL);
    lv_obj_add_style(gameScreen, &style_bg, 0);
    // Desactivar scroll en pantalla principal
    lv_obj_clear_flag(gameScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(gameScreen, LV_SCROLLBAR_MODE_OFF);
    
    // Score label (top center)
    scoreLabel = lv_label_create(gameScreen);
    lv_obj_add_style(scoreLabel, &style_score, 0);
    lv_label_set_text(scoreLabel, "SCORE: 0");
    lv_obj_align(scoreLabel, LV_ALIGN_TOP_MID, 0, 20);
    
    // Lives label removed
    
    
    // Pause overlay (hidden initially)
    pauseOverlay = lv_obj_create(gameScreen);
    lv_obj_set_size(pauseOverlay, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(pauseOverlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(pauseOverlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(pauseOverlay, 0, 0);
    
    lv_obj_t *pauseText = lv_label_create(pauseOverlay);
    lv_obj_add_style(pauseText, &style_title, 0);
    lv_label_set_text(pauseText, "PAUSED");
    lv_obj_align(pauseText, LV_ALIGN_CENTER, 0, 0);
    
    lv_obj_add_flag(pauseOverlay, LV_OBJ_FLAG_HIDDEN);
}

void initGame() {
    // Initialize paddle (horizontal at bottom)
    float paddleWidth = std::min<float>(PADDLE_WIDTH, SCREEN_WIDTH * 0.35f); // cap relative to width
    game.paddle = {SCREEN_WIDTH/2.0f - paddleWidth/2.0f, (float)SCREEN_HEIGHT - 40.0f, paddleWidth, PADDLE_HEIGHT, PADDLE_SPEED};
    
    // Initialize ball
    resetBall();
    
    // Game state
    game.score = 0;
    game.gameSpeed = 1.0f;
    game.ballLaunched = false;
    game.lastUpdate = millis();
    game.difficulty = 1.0f;
    game.lastScoreTime = millis();
    game.bounceCount = 0; // Inicializar contador de rebotes
    game.currentBallSpeed = INITIAL_BALL_SPEED; // Velocidad inicial
}

void resetBall() {
    if (ballOnMyScreen) {
        game.ball.x = game.paddle.x + game.paddle.width / 2;
        game.ball.y = game.paddle.y - game.ball.radius - 5;
        game.ball.vx = 0;
        game.ball.vy = 0;
        game.ball.radius = BALL_RADIUS;
        game.ball.speed = game.currentBallSpeed; // Usar velocidad actual
        game.ballLaunched = false;
    } else {
        // Si no tengo la pelota, posicionarla fuera de pantalla
        game.ball.x = -100;
        game.ball.y = -100;
        game.ball.vx = 0;
        game.ball.vy = 0;
    game.ball.radius = BALL_RADIUS; // Asegurar radio definido para futura recepción
        game.ballLaunched = false;
    }
}

void resetGame() {
    game.score = 0;
    game.gameSpeed = 1.0f;
    game.difficulty = 1.0f;
    game.bounceCount = 0; // Reiniciar contador
    game.currentBallSpeed = INITIAL_BALL_SPEED; // Reiniciar velocidad
    resetBall();
}

void updateGame() {
    uint32_t now = millis();
    float dt = (now - game.lastUpdate) / 1000.0f;
    game.lastUpdate = now;
    
    // Update paddle
    updatePaddle(game.paddle);
    
    // Update ball
    if (game.ballLaunched) {
        game.ball.x += game.ball.vx * dt;
        game.ball.y += game.ball.vy * dt;
        
        // Quitar fricción continua
        // game.ball.vx *= FRICTION;
        // game.ball.vy *= FRICTION;
        
        // Check collisions
        checkCollisions();
        
        // Check if ball is out of bounds (bottom) and simply reset position
        if (game.ball.y > SCREEN_HEIGHT) {
            resetBall();
        }
        
    // Ya no reseteamos al tocar techo; el puntaje se gestiona en checkCollisions sobre rebote real
    }
}

void updatePaddle(Paddle &paddle) {
    // Keep inside horizontal limits considering rounded corners (avoid entering cut corners)
    float minX = 0.0f;
    float maxX = (float)SCREEN_WIDTH - paddle.width;
    if (paddle.x < minX) paddle.x = minX;
    if (paddle.x > maxX) paddle.x = maxX;
}

void checkCollisions() {
    // Rectangular simple collisions
    if (game.ball.x <= game.ball.radius) {
        game.ball.x = game.ball.radius;
        game.ball.vx = -game.ball.vx * WALL_BOUNCE;
    } else if (game.ball.x >= SCREEN_WIDTH - game.ball.radius) {
        game.ball.x = SCREEN_WIDTH - game.ball.radius;
        game.ball.vx = -game.ball.vx * WALL_BOUNCE;
    }
    if (game.ball.y <= game.ball.radius && game.ball.vy < 0) {
        game.ball.y = game.ball.radius;
        game.ball.vy = fabsf(game.ball.vy) * WALL_BOUNCE;
        if (game.ball.vy < 40.0f) game.ball.vy = 40.0f;
        
        // Incremento progresivo de velocidad por rebote en el techo
        game.bounceCount++;
        if (game.bounceCount % (int)DIFFICULTY_THRESHOLD == 0) {
            game.currentBallSpeed = std::min(game.currentBallSpeed + SPEED_INCREMENT, MAX_BALL_SPEED);
            // Actualizar velocidad actual manteniendo dirección
            float currentSpeed = sqrtf(game.ball.vx * game.ball.vx + game.ball.vy * game.ball.vy);
            if (currentSpeed > 0) {
                float speedMultiplier = game.currentBallSpeed / currentSpeed;
                game.ball.vx *= speedMultiplier;
                game.ball.vy *= speedMultiplier;
            }
        }
        
        game.score += 10;
        if (game.score % 50 == 0) {
            game.difficulty += 0.2f;
            game.gameSpeed = 1.0f + (game.difficulty - 1.0f) * 0.3f;
        }
    }

    // 3. Paddle collision (AABB with circle center approximation)
    if (game.ball.y + game.ball.radius >= game.paddle.y &&
        game.ball.y - game.ball.radius <= game.paddle.y + game.paddle.height &&
        game.ball.x + game.ball.radius >= game.paddle.x &&
        game.ball.x - game.ball.radius <= game.paddle.x + game.paddle.width) {
        // Position ball just above paddle
        game.ball.y = game.paddle.y - game.ball.radius;
        float hitPos = (game.ball.x - game.paddle.x) / game.paddle.width; // 0..1
        if (hitPos < 0) hitPos = 0; if (hitPos > 1) hitPos = 1;
        float angle = (hitPos - 0.5f) * 1.4f; // spread
        float speed = game.currentBallSpeed; // Usar velocidad actual en lugar de calcular
        game.ball.vx = sinf(angle) * speed;
        game.ball.vy = -cosf(angle) * speed; // upward
        // slight random tweak
        game.ball.vx += random(-10,11);
        // Guarantee upward motion
        if (game.ball.vy > -60) game.ball.vy = -60; // mayor componente vertical para evitar trayectorias casi horizontales
    }
}

void drawGame() {
    // Crear objetos si aún no existen (evitar borrar la pantalla para no perder pauseOverlay)
    if (!fieldObj) {
        fieldObj = lv_obj_create(gameScreen);
        lv_obj_set_size(fieldObj, SCREEN_WIDTH, SCREEN_HEIGHT);
        lv_obj_set_style_bg_color(fieldObj, lv_color_hex(0x000000), 0);
        lv_obj_set_style_radius(fieldObj, 0, 0);
        lv_obj_set_style_border_width(fieldObj, 0, 0);
        lv_obj_set_style_border_color(fieldObj, COLOR_LINE, 0);
        lv_obj_set_pos(fieldObj, 0, 0);
        lv_obj_clear_flag(fieldObj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(fieldObj, LV_SCROLLBAR_MODE_OFF);
    }
    if (!scoreLabel) {
        scoreLabel = lv_label_create(gameScreen);
        lv_obj_add_style(scoreLabel, &style_score, 0);
        lv_obj_align(scoreLabel, LV_ALIGN_TOP_MID, 0, 30);
    }
    if (!paddleObj) {
        paddleObj = lv_obj_create(gameScreen);
        lv_obj_set_size(paddleObj, game.paddle.width, game.paddle.height);
        lv_obj_set_style_bg_color(paddleObj, COLOR_PADDLE, 0);
        lv_obj_set_style_radius(paddleObj, 3, 0);
        lv_obj_clear_flag(paddleObj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(paddleObj, LV_SCROLLBAR_MODE_OFF);
    }
    if (!ballObj) {
        ballObj = lv_obj_create(gameScreen);
        lv_obj_set_size(ballObj, game.ball.radius * 2, game.ball.radius * 2);
        lv_obj_set_style_bg_color(ballObj, COLOR_BALL, 0);
        lv_obj_set_style_radius(ballObj, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(ballObj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(ballObj, LV_SCROLLBAR_MODE_OFF);
    }
    
    // Actualiza posiciones y textos
    lv_label_set_text_fmt(scoreLabel, "SCORE: %d", game.score);
    // Lives removed
    lv_obj_set_pos(paddleObj, game.paddle.x, game.paddle.y);
    
    // Mostrar/ocultar y posicionar la pelota según el estado
    if (ballOnMyScreen && game.ballLaunched) {
        lv_obj_set_pos(ballObj, game.ball.x - game.ball.radius, game.ball.y - game.ball.radius);
        lv_obj_clear_flag(ballObj, LV_OBJ_FLAG_HIDDEN);
        if (DEBUG_DRAW_BALL) Serial.printf("[DRAW] Ball visible at (%.1f,%.1f)\n", game.ball.x, game.ball.y);
    } else if (ballOnMyScreen && !game.ballLaunched) {
        lv_obj_set_pos(ballObj, game.ball.x - game.ball.radius, game.ball.y - game.ball.radius);
        lv_obj_clear_flag(ballObj, LV_OBJ_FLAG_HIDDEN);
        if (DEBUG_DRAW_BALL) Serial.printf("[DRAW] Ball waiting at paddle (%.1f,%.1f)\n", game.ball.x, game.ball.y);
    } else {
        lv_obj_add_flag(ballObj, LV_OBJ_FLAG_HIDDEN);
        if (DEBUG_DRAW_BALL) Serial.println("[DRAW] Ball hidden - not on my screen");
    }
    
    // El overlay de pausa global se controla sólo desde handleInput.
}

// Helper function for IO expander
static inline bool isPressed(uint8_t pinIndex) {
    int v = ioexp.read1(pinIndex);
    return (v != TCA9555_INVALID_READ) && (v == LOW);
}

void handleInput() {
    // Función auxiliar para asegurar existencia del overlay
    auto ensurePauseOverlay = [](){
        if (!pauseOverlay || !lv_obj_is_valid(pauseOverlay)) {
            pauseOverlay = lv_obj_create(gameScreen);
            lv_obj_set_size(pauseOverlay, SCREEN_WIDTH, SCREEN_HEIGHT);
            lv_obj_set_style_bg_color(pauseOverlay, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(pauseOverlay, LV_OPA_50, 0);
            lv_obj_set_style_border_width(pauseOverlay, 0, 0);
            lv_obj_t *pauseText = lv_label_create(pauseOverlay);
            lv_obj_add_style(pauseText, &style_title, 0);
            lv_label_set_text(pauseText, "PAUSED");
            lv_obj_align(pauseText, LV_ALIGN_CENTER, 0, 0);
            lv_obj_add_flag(pauseOverlay, LV_OBJ_FLAG_HIDDEN);
            Serial.println("[SAFE] Re-created pauseOverlay");
        }
    };
    ensurePauseOverlay();

    // TOP button for pause/resume
    static bool topWasPressed = false;
    bool topNow = digitalRead(BUTTON_TOP) == LOW;
    
    if (topNow && !topWasPressed && (gameState == PLAYING || gameState == MULTIPLAYER_PLAYING)) {
        gameState = PAUSED;
        ensurePauseOverlay();
        if (pauseOverlay && lv_obj_is_valid(pauseOverlay)) lv_obj_clear_flag(pauseOverlay, LV_OBJ_FLAG_HIDDEN);
    } else if (topNow && !topWasPressed && gameState == PAUSED) {
        // Resume to appropriate game mode
        gameState = isConnected ? MULTIPLAYER_PLAYING : PLAYING;
        if (pauseOverlay && lv_obj_is_valid(pauseOverlay)) lv_obj_add_flag(pauseOverlay, LV_OBJ_FLAG_HIDDEN);
    }
    topWasPressed = topNow;
    
    // Movimiento más suave de la paleta con menor velocidad
    if (gameState == PLAYING || gameState == PAUSED || gameState == MULTIPLAYER_PLAYING) {
        if (isPressed(EXPANDER_PAD_LEFT)) {
            game.paddle.x -= game.paddle.speed * 0.012f; // Reducido de 0.016f para movimiento más cómodo
        } else if (isPressed(EXPANDER_PAD_RIGHT)) {
            game.paddle.x += game.paddle.speed * 0.012f; // Reducido de 0.016f para movimiento más cómodo
        }
        updatePaddle(game.paddle);
    }
    
    // Launch ball with any pad press if not launched
    if (!game.ballLaunched && gameState == PLAYING) {
        if (isPressed(EXPANDER_PAD_TOP) || isPressed(EXPANDER_PAD_BOTTOM) || 
            isPressed(EXPANDER_PAD_LEFT) || isPressed(EXPANDER_PAD_RIGHT)) {
            game.ballLaunched = true;
            game.ball.vx = random(-30, 30); // Menor variabilidad inicial
            game.ball.vy = -game.currentBallSpeed; // Usar velocidad actual
        }
    }
    
    // Launch ball in multiplayer
    if (!game.ballLaunched && gameState == MULTIPLAYER_PLAYING && ballOnMyScreen) {
        if (isPressed(EXPANDER_PAD_TOP) || isPressed(EXPANDER_PAD_BOTTOM) || 
            isPressed(EXPANDER_PAD_LEFT) || isPressed(EXPANDER_PAD_RIGHT)) {
            game.ballLaunched = true;
            game.ball.vx = random(-30, 30);
            game.ball.vy = -game.currentBallSpeed;
        }
    }
}

// ESP-NOW Functions

String generateDeviceID() {
    // Use ESP32 chip ID for guaranteed uniqueness
    uint64_t chipid = ESP.getEfuseMac();
    return String((uint32_t)(chipid >> 16), HEX);
}

String generateRoomID() {
    // Generate a 4-character room ID based on device ID
    String room = deviceID.substring(0, 4);
    room.toUpperCase();
    return room;
}

void startAsHost() {
    Serial.println("=== STARTING AS HOST ===");
    roomID = generateRoomID();
    gameState = HOST_WAITING;
    isHost = true;
    isConnected = false;
    ballOnMyScreen = true;
    
    Serial.printf("Generated Room ID: %s\n", roomID.c_str());
    
    // Update room ID in the UI
    lv_scr_load(hostWaitingScreen);
    if (hostRoomIdLabel) {
        lv_label_set_text(hostRoomIdLabel, roomID.c_str());
    }
    
    // Start announcing the room
    announceRoom();
}

void startAsClient() {
    Serial.println("=== STARTING AS CLIENT ===");
    gameState = CLIENT_SEARCHING;
    isHost = false;
    isConnected = false;
    ballOnMyScreen = false;
    availableRooms.clear();
    
    lv_scr_load(clientSearchingScreen);
    
    // Start searching for rooms
    searchForRooms();
}

void announceRoom() {
    Serial.printf("Announcing room: %s\n", roomID.c_str());
    
    uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    // Remove any existing broadcast peer first
    esp_now_del_peer(broadcastMAC);
    
    // Add broadcast peer
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, broadcastMAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    
    esp_err_t addResult = esp_now_add_peer(&peerInfo);
    if (addResult == ESP_OK || addResult == ESP_ERR_ESPNOW_EXIST) {
        ESPNowMessage msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_ROOM_ANNOUNCE;
        strcpy(msg.roomID, roomID.c_str());
        strcpy(msg.deviceID, deviceID.c_str());
        
        esp_err_t result = esp_now_send(broadcastMAC, (uint8_t*)&msg, sizeof(msg));
        Serial.printf("Room announce result: %d\n", result);
    }
}

void searchForRooms() {
    Serial.println("Searching for available rooms...");
    
    uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    // Remove any existing broadcast peer first
    esp_now_del_peer(broadcastMAC);
    
    // Add broadcast peer
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, broadcastMAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    
    esp_err_t addResult = esp_now_add_peer(&peerInfo);
    if (addResult == ESP_OK || addResult == ESP_ERR_ESPNOW_EXIST) {
        ESPNowMessage msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_ROOM_SEARCH;
        strcpy(msg.deviceID, deviceID.c_str());
        
        esp_err_t result = esp_now_send(broadcastMAC, (uint8_t*)&msg, sizeof(msg));
        Serial.printf("Room search result: %d\n", result);
    }
}

void joinRoom(String roomId) {
    Serial.printf("Attempting to join room: %s\n", roomId.c_str());
    
    // This will be handled when we receive a room announce with the matching ID
    // For now, we'll send a join request broadcast
    uint8_t broadcastMAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    ESPNowMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_JOIN_REQUEST;
    strcpy(msg.roomID, roomId.c_str());
    strcpy(msg.deviceID, deviceID.c_str());
    
    esp_now_send(broadcastMAC, (uint8_t*)&msg, sizeof(msg));
}

void initESPNow() {
    // Generate unique device ID from MAC
    deviceID = generateDeviceID();
    Serial.printf("Device ID: %s\n", deviceID.c_str());
    
    // Initialize WiFi in STA mode with proper configuration
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    
    // Wait for WiFi to be ready
    delay(100);
    
    // Print MAC address for debugging
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    Serial.printf("MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    // Initialize ESP-NOW
    esp_err_t result = esp_now_init();
    if (result != ESP_OK) {
        Serial.printf("Error initializing ESP-NOW: %d\n", result);
        return;
    }
    
    // Register callbacks
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataReceive);
    
    Serial.println("ESP-NOW initialized successfully");
}

void onDataReceive(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    Serial.printf("Data received from: %02X:%02X:%02X:%02X:%02X:%02X, len: %d\n", 
                  recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
                  recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5], len);
    
    Serial.printf("Expected size: %d, Actual size: %d\n", (int)sizeof(ESPNowMessage), len);
    
    if (len != sizeof(ESPNowMessage)) {
        Serial.printf("*** SIZE MISMATCH *** Expected: %d, Got: %d\n", sizeof(ESPNowMessage), len);
        // Let's try to process anyway to see what we get
    }
    
    const uint8_t *mac = recv_info->src_addr;
    ESPNowMessage msg;
    memcpy(&msg, data, min(len, (int)sizeof(msg)));
    
    Serial.printf("Message type: %d\n", msg.type);
    
    // Debug: Print raw message bytes
    if (DEBUG_RAW_BYTES) {
        Serial.print("Raw bytes: ");
        for(int i = 0; i < min(len, 64); i++) {
            Serial.printf("%02X ", data[i]);
        }
        Serial.println();
    }
    
    switch (msg.type) {
        case MSG_ROOM_ANNOUNCE:
            Serial.printf("Received ROOM_ANNOUNCE from room: %s\n", msg.roomID);
            if (gameState == CLIENT_SEARCHING) {
                // Add room to available rooms if not already present
                String room = String(msg.roomID);
                bool found = false;
                for (const auto& existingRoom : availableRooms) {
                    if (existingRoom == room) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    availableRooms.push_back(room);
                    Serial.printf("Added room to list: %s\n", room.c_str());
                    
                    // Add room to UI list
                    lv_obj_t *btn = lv_list_add_btn(roomList, NULL, room.c_str());
                    // Aumentar fuente (doble) del label del botón
                    lv_obj_t *btnLabel = lv_obj_get_child(btn, 0);
                    if (btnLabel) {
                        // Reducir fuente (más pequeña) según nueva petición
                        lv_obj_set_style_text_font(btnLabel, &Inter_40, 0);
                    }
                    // Centrar verticalmente tras primera inserción aproximando scroll
                    lv_obj_scroll_to_y(roomList, 0, LV_ANIM_OFF);
                    lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                        lv_obj_t *btn = (lv_obj_t*)lv_event_get_target(e);
                        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
                            const char *roomText = lv_list_get_btn_text(roomList, btn);
                            joinRoom(String(roomText));
                        }
                    }, LV_EVENT_CLICKED, NULL);
                }
            }
            break;
            
        case MSG_ROOM_SEARCH:
            Serial.printf("Received ROOM_SEARCH from device: %s\n", msg.deviceID);
            if (gameState == HOST_WAITING && !isConnected) {
                // Respond with our room announcement
                announceRoom();
            }
            break;
            
        case MSG_JOIN_REQUEST:
            Serial.printf("Received JOIN_REQUEST for room: %s from device: %s\n", msg.roomID, msg.deviceID);
            if (gameState == HOST_WAITING && !isConnected && String(msg.roomID) == roomID) {
                Serial.println("Valid join request received");
                // Add sender as peer
                esp_now_peer_info_t peerInfo;
                memset(&peerInfo, 0, sizeof(peerInfo));
                memcpy(peerInfo.peer_addr, mac, 6);
                peerInfo.channel = 0;
                peerInfo.encrypt = false;
                peerInfo.ifidx = WIFI_IF_STA;
                
                esp_err_t result = esp_now_add_peer(&peerInfo);
                if (result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST) {
                    memcpy(peerMAC, mac, 6);
                    isConnected = true;
                    
                    // Host decides who starts with the ball randomly
                    bool hostStartsWithBall = random(0, 2) == 0;
                    ballOnMyScreen = hostStartsWithBall;
                    
                    // Send response with initial ball assignment
                    ESPNowMessage response;
                    memset(&response, 0, sizeof(response));
                    response.type = MSG_JOIN_RESPONSE;
                    strcpy(response.roomID, roomID.c_str());
                    strcpy(response.deviceID, deviceID.c_str());
                    response.ballActive = !hostStartsWithBall; // Client gets ball if host doesn't start
                    
                    esp_now_send(mac, (uint8_t*)&response, sizeof(response));
                    Serial.printf("Join accepted, connection established as HOST. Ball assignment - Host: %s, Client: %s\n", 
                                  ballOnMyScreen ? "HAS BALL" : "WAITING", 
                                  !ballOnMyScreen ? "HAS BALL" : "WAITING");
                }
            }
            break;
            
        case MSG_JOIN_RESPONSE:
            Serial.printf("Received JOIN_RESPONSE from room: %s\n", msg.roomID);
            if (gameState == CLIENT_SEARCHING && !isConnected) {
                // Add sender as peer
                esp_now_peer_info_t peerInfo;
                memset(&peerInfo, 0, sizeof(peerInfo));
                memcpy(peerInfo.peer_addr, mac, 6);
                peerInfo.channel = 0;
                peerInfo.encrypt = false;
                peerInfo.ifidx = WIFI_IF_STA;
                
                esp_err_t result = esp_now_add_peer(&peerInfo);
                if (result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST) {
                    memcpy(peerMAC, mac, 6);
                    isConnected = true;
                    roomID = String(msg.roomID);
                    ballOnMyScreen = msg.ballActive; // Receive ball assignment from host
                    Serial.printf("Successfully joined room: %s as CLIENT. Ball assignment: %s\n", 
                                  roomID.c_str(), ballOnMyScreen ? "I HAVE BALL" : "WAITING FOR BALL");
                }
            }
            break;
            
        case MSG_BALL_TRANSFER: {
            if (DEBUG_BALL_TRANSFER) Serial.println("=== RECEIVED BALL_TRANSFER ===");
            Serial.printf("My state - connected:%d, ballOnMyScreen:%d, gameState:%d\n", 
                          isConnected, ballOnMyScreen, gameState);
            if (isConnected && !ballOnMyScreen) {
                // Aplicar datos recibidos
                ballOnMyScreen = true;
                // Asegurar radio válido
                if (game.ball.radius <= 0.0f) game.ball.radius = BALL_RADIUS;
                // Espejar horizontalmente porque las pantallas están enfrentadas.
                float mirroredX = (float)SCREEN_WIDTH - msg.ballX;
                // Ajustar para mantener dentro de límites (centro dentro)
                if (mirroredX < game.ball.radius) mirroredX = game.ball.radius;
                if (mirroredX > SCREEN_WIDTH - game.ball.radius) mirroredX = SCREEN_WIDTH - game.ball.radius;
                game.ball.x = mirroredX;
                // Entrada a la pantalla en el borde superior (ligeramente dentro para evitar rebote inmediato)
                game.ball.y = game.ball.radius + 1;
                // Dirección descendente garantizada (pantallas enfrentadas por el lado superior)
                float speedMag = sqrtf(msg.ballVX * msg.ballVX + msg.ballVY * msg.ballVY);
                if (speedMag < 1.0f) speedMag = msg.currentBallSpeed; // fallback
                // Mantener el componente horizontal y forzar vertical positiva
                // Invertir componente horizontal para coherencia visual entre pantallas enfrentadas
                game.ball.vx = -msg.ballVX;
                game.ball.vy = fabsf(msg.ballVY);
                // Sincronizar velocidad progresiva
                game.currentBallSpeed = msg.currentBallSpeed;
                // Normalizar a currentBallSpeed para consistencia
                float newMag = sqrtf(game.ball.vx * game.ball.vx + game.ball.vy * game.ball.vy);
                if (newMag > 0.0f) {
                    float m = game.currentBallSpeed / newMag;
                    game.ball.vx *= m;
                    game.ball.vy *= m;
                }
                // Forzar que la pelota esté en movimiento al llegar
                game.ballLaunched = true;
                // Reiniciar delta tiempo para evitar salto
                game.lastUpdate = millis();
                // Redimensionar objeto gráfico si el radio no coincidía (por seguridad)
                if (ballObj) {
                    lv_obj_set_size(ballObj, game.ball.radius * 2, game.ball.radius * 2);
                }
                if (DEBUG_BALL_TRANSFER) Serial.printf("Ball received -> pos(%.1f,%.1f) vel(%.1f,%.1f) speedRef:%.1f active:%d\n", 
                              game.ball.x, game.ball.y, game.ball.vx, game.ball.vy, game.currentBallSpeed, game.ballLaunched);
                // Redibujar inmediatamente
                if (gameState == MULTIPLAYER_PLAYING) {
                    drawGame();
                }
            } else {
                if (DEBUG_BALL_TRANSFER) Serial.printf("Ball transfer IGNORED - connected:%d, ballOnMyScreen:%d\n", isConnected, ballOnMyScreen);
            }
            break;
        }
            
        case MSG_GAME_UPDATE:
            Serial.println("Received GAME_UPDATE");
            // Update peer's paddle position (visual only)
            // In this simple implementation, we don't show peer's paddle
            break;
            
        case MSG_HEARTBEAT:
            Serial.println("Received HEARTBEAT");
            lastHeartbeat = millis();
            break;
            
        default:
            Serial.printf("Unknown message type: %d\n", msg.type);
            break;
    }
}

void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    Serial.printf("Send to %02X:%02X:%02X:%02X:%02X:%02X: %s\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAILED");
}

void sendGameUpdate() {
    if (!isConnected) return;
    
    ESPNowMessage msg;
    msg.type = MSG_GAME_UPDATE;
    msg.paddleX = game.paddle.x;
    msg.score = game.score;
    
    esp_now_send(peerMAC, (uint8_t*)&msg, sizeof(msg));
}

void sendBallTransfer() {
    if (!isConnected || !ballOnMyScreen) {
        Serial.printf("Ball transfer blocked - connected:%d, ballOnMyScreen:%d\n", isConnected, ballOnMyScreen);
        return;
    }
    
    Serial.println("=== PREPARING BALL TRANSFER ===");
    Serial.printf("ESPNowMessage size: %d bytes\n", (int)sizeof(ESPNowMessage));
    
    ESPNowMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_BALL_TRANSFER;
    msg.ballX = game.ball.x; // Keep X position
    msg.ballY = game.ball.y; // Y position (will be used to position at top of other screen)
    msg.ballVX = game.ball.vx; // Mantener componente X
    msg.ballVY = game.ball.vy; // Será invertido a descendente en receptor
    msg.currentBallSpeed = game.currentBallSpeed; // sincronizar progresión de velocidad
    msg.ballActive = game.ballLaunched;
    strcpy(msg.roomID, roomID.c_str());
    strcpy(msg.deviceID, deviceID.c_str());
    
    Serial.printf("MSG_BALL_TRANSFER = %d\n", MSG_BALL_TRANSFER);
    Serial.printf("Message size: %d bytes\n", sizeof(msg));
    Serial.printf("Sending ball: pos(%.1f,%.1f) vel(%.1f,%.1f) active:%d\n", 
                  msg.ballX, msg.ballY, msg.ballVX, msg.ballVY, msg.ballActive);
    Serial.printf("Peer MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                  peerMAC[0], peerMAC[1], peerMAC[2], peerMAC[3], peerMAC[4], peerMAC[5]);
    
    // Debug: Print raw message bytes
    if (DEBUG_RAW_BYTES) {
        Serial.print("Sending bytes: ");
        uint8_t* msgBytes = (uint8_t*)&msg;
        for(int i = 0; i < (int)sizeof(msg); i++) {
            Serial.printf("%02X ", msgBytes[i]);
        }
        Serial.println();
    }
    
    // Try sending multiple times to ensure delivery
    for (int i = 0; i < 3; i++) {
        esp_err_t result = esp_now_send(peerMAC, (uint8_t*)&msg, sizeof(msg));
        Serial.printf("Send attempt %d: %s\n", i+1, result == ESP_OK ? "SUCCESS" : "FAILED");
        if (result == ESP_OK) {
            break;
        }
        delay(10);
    }
    
    // Dejar de poseer la pelota inmediatamente
    ballOnMyScreen = false;
    // Evitar que siga avanzando localmente accidentalmente
    game.ballLaunched = false;
    Serial.println("Ball transfer completed, ball no longer on my screen");
    
    // Force immediate visual update to hide the ball
    if (gameState == MULTIPLAYER_PLAYING) {
        drawGame();
        Serial.println("Forced visual update after sending ball");
    }
}

void updateMultiplayerGame() {
    uint32_t now = millis();
    float dt = (now - game.lastUpdate) / 1000.0f;
    game.lastUpdate = now;
    
    // Log ball state periodically
    static uint32_t lastBallLog = 0;
    if (now - lastBallLog > 2000) {
        Serial.printf("[UPDATE] Ball state - onMyScreen:%d, launched:%d, pos(%.1f,%.1f)\n", 
                      ballOnMyScreen, game.ballLaunched, game.ball.x, game.ball.y);
        lastBallLog = now;
    }
    
    // Update paddle
    updatePaddle(game.paddle);
    
    // Update ball only if it's on my screen
    if (ballOnMyScreen && game.ballLaunched) {
        game.ball.x += game.ball.vx * dt;
        game.ball.y += game.ball.vy * dt;
        
        // Check collisions
        checkMultiplayerCollisions();
        
        // Check if ball goes off top of screen - transfer to peer
        // Transferir un poco antes de salir completamente para continuidad
        if ((game.ball.y + game.ball.radius) < 0.0f && game.ball.vy < 0) {
            if (DEBUG_BALL_TRANSFER) Serial.printf("[TRANSFER] Exiting top (y=%.2f) -> sending to peer\n", game.ball.y);
            sendBallTransfer();
        }
        
        // Check if ball is out of bounds (bottom) - reset on my screen
        if (game.ball.y > SCREEN_HEIGHT) {
            Serial.printf("[UPDATE] Ball hit bottom at y=%.1f, resetting\n", game.ball.y);
            resetBall();
        }
    }
    
    // Send heartbeat periodically
    static uint32_t lastHeartbeatSent = 0;
    if (now - lastHeartbeatSent > 1000) {
        if (isConnected) {
            ESPNowMessage msg;
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_HEARTBEAT;
            esp_now_send(peerMAC, (uint8_t*)&msg, sizeof(msg));
        }
        lastHeartbeatSent = now;
    }
}

void checkMultiplayerCollisions() {
    // Wall collisions (left/right only)
    if (game.ball.x <= game.ball.radius) {
        game.ball.x = game.ball.radius;
        game.ball.vx = -game.ball.vx * WALL_BOUNCE;
    } else if (game.ball.x >= SCREEN_WIDTH - game.ball.radius) {
        game.ball.x = SCREEN_WIDTH - game.ball.radius;
        game.ball.vx = -game.ball.vx * WALL_BOUNCE;
    }
    
    // No top wall collision - ball transfers to peer instead
    
    // Paddle collision
    if (game.ball.y + game.ball.radius >= game.paddle.y &&
        game.ball.y - game.ball.radius <= game.paddle.y + game.paddle.height &&
        game.ball.x + game.ball.radius >= game.paddle.x &&
        game.ball.x - game.ball.radius <= game.paddle.x + game.paddle.width) {
        
        // Position ball just above paddle
        game.ball.y = game.paddle.y - game.ball.radius;
        float hitPos = (game.ball.x - game.paddle.x) / game.paddle.width;
        if (hitPos < 0) hitPos = 0; 
        if (hitPos > 1) hitPos = 1;
        float angle = (hitPos - 0.5f) * 1.4f;
        float speed = game.currentBallSpeed;
        game.ball.vx = sinf(angle) * speed;
        game.ball.vy = -cosf(angle) * speed;
        game.ball.vx += random(-10,11);
        if (game.ball.vy > -60) game.ball.vy = -60;
        
        // Increase score
        game.score += 10;
    }
}

// gameOver removed (no lives system)