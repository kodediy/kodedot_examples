/*
 * Slides Remote Control for Kode Dot (ESP32-S3)
 * ------------------------------------------------------------
 * Purpose:
 *  - Control presentation slides via BLE HID keyboard
 *  - Built-in timer for presentation timing
 *  - Clean interface showing slide number and connection status
 *
 * Features:
 *  - BLE HID keyboard for slide navigation
 *  - Physical buttons and touch pad for control
 *  - Presentation timer with play/pause/reset
 *  - Battery monitoring
 *  - LVGL-based user interface
 *
 * Controls:
 *  - Top buttons: Next slide
 *  - Bottom buttons: Previous slide
 *  - Left buttons: Toggle timer
 *  - Right buttons: Reset timer
 */
#include <Arduino.h>
#include <lvgl.h>
#include <kodedot/display_manager.h>
#include <BleKeyboard.h>
#include <kodedot/pin_config.h>
#include <Wire.h>
#include <TCA9555.h>
#include <Adafruit_MAX1704X.h>

// Declarar las fuentes personalizadas
extern const lv_font_t Inter_100;
extern const lv_font_t Inter_70;
extern const lv_font_t Inter_200;

// Declarar la imagen del logotipo
extern const lv_image_dsc_t logotipo;

// Instancia del gestor de display
DisplayManager display;

// BLE HID instance
BleKeyboard bleKeyboard("MediaRemote", "Luis", 100);

// TCA9555 expander instance
TCA9555 ioexpander(IOEXP_I2C_ADDR);

// Sensor de batería
Adafruit_MAX17048 batteryMonitor;

// Estados de botones para detección de flancos
static uint8_t lastTop = HIGH;
static uint8_t lastBottom = HIGH;
static uint8_t lastLeft = HIGH;
static uint8_t lastRight = HIGH;

// Variables de estado
static int slideNumber = 1;
static bool timerRunning = false;
static unsigned long timerStartMs = 0;
static unsigned long elapsedMs = 0;
static bool bleWasConnected = false;

// Objetos de la interfaz LVGL
static lv_obj_t *labelStatus;
static lv_obj_t *labelSlide;
static lv_obj_t *labelTimer;

// Declaraciones de funciones
void createSlideInterface();
void updateBLEStatus();
void updateTimerDisplay();
void handlePhysicalButtons();
void handleTouchButtons();
void nextSlide();
void prevSlide();
void toggleTimer();
void resetTimer();

void setup() {
    Serial.begin(115200);
    Serial.println("Iniciando Control de Diapositivas...");

    // Inicializar I2C para TCA9555 y sensores
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    
    // Inicializar el expansor de E/S
    if (!ioexpander.begin(INPUT)) {
        Serial.println("Error: TCA9555 no detectado");
    } else {
        // Configurar pines del expansor como entrada (ya configurados en begin con INPUT)
        Serial.println("TCA9555 inicializado correctamente");
    }

    // Configurar botones físicos
    pinMode(BUTTON_TOP, INPUT_PULLUP);

    // Inicializar el sistema de display
    if (!display.init()) {
        Serial.println("Error: No se pudo inicializar el display");
        while(1) {
            delay(1000);
        }
    }

    // Inicializar sensor de batería
    if (!batteryMonitor.begin(&Wire)) {
        Serial.println("Error: MAX17048 no detectado");
    } else {
        batteryMonitor.quickStart();
        Serial.println("Sensor de batería inicializado");
    }

    // Inicializar BLE
    bleKeyboard.begin();
    Serial.println("BLE Keyboard iniciado");

    // Crear la interfaz de usuario
    createSlideInterface();
    
    Serial.println("Sistema listo!");
}

void loop() {
    // Actualizar el sistema de display
    display.update();
    
    // Actualizar estados
    updateBLEStatus();
    updateTimerDisplay();
    
    // Manejar botones físicos y del expansor
    handlePhysicalButtons();
    
    // Manejar eventos touch
    handleTouchButtons();
    
    delay(10);
}

/**
 * @brief Crea la interfaz de usuario para control de diapositivas
 */
void createSlideInterface() {
    lv_obj_t * scr = lv_scr_act();
    
    // ========================================
    // CONFIGURACIÓN VISUAL - FÁCIL DE MODIFICAR
    // ========================================
    
    // --- COLORES ---
    const lv_color_t COLOR_FONDO = lv_color_hex(0x000000);           // Fondo negro
    const lv_color_t COLOR_LOGO = lv_color_hex(0xFFFFFF);            // Logo (se usa automáticamente)
    const lv_color_t COLOR_NUMERO_DIAPOSITIVA = lv_color_hex(0xFF7F1F); // Naranja de marca
    const lv_color_t COLOR_TEMPORIZADOR = lv_color_hex(0xFFFAF5);    // Blanco roto
    const lv_color_t COLOR_BLE_CONECTADO = lv_color_hex(0x00FF88);   // Verde claro
    const lv_color_t COLOR_BLE_DESCONECTADO = lv_color_hex(0x9A948F); // Gris
    
    // --- POSICIONES (X, Y) ---
    // Logo (arriba)
    const lv_align_t LOGO_ALINEACION = LV_ALIGN_TOP_MID;
    const int LOGO_OFFSET_X = 10;
    const int LOGO_OFFSET_Y = 20;
    
    // Estado BLE
    const lv_align_t BLE_ALINEACION = LV_ALIGN_TOP_LEFT;
    const int BLE_OFFSET_X = 40;
    const int BLE_OFFSET_Y = 40;
    
    // Número de diapositiva (centro)
    const lv_align_t SLIDE_ALINEACION = LV_ALIGN_CENTER;
    const int SLIDE_OFFSET_X = 0;
    const int SLIDE_OFFSET_Y = 0;
    
    // Temporizador
    const lv_align_t TIMER_ALINEACION = LV_ALIGN_BOTTOM_MID;
    const int TIMER_OFFSET_X = 0;
    const int TIMER_OFFSET_Y = -40;
    
    // --- FUENTES ---
    const lv_font_t* FUENTE_BLE = &lv_font_montserrat_18;
    const lv_font_t* FUENTE_NUMERO = &Inter_200;
    const lv_font_t* FUENTE_TIMER = &Inter_70;
    
    // ========================================
    // CREACIÓN DE ELEMENTOS (NO TOCAR)
    // ========================================
    
    // Establecer color de fondo
    lv_obj_set_style_bg_color(scr, COLOR_FONDO, 0);

    // Logotipo
    lv_obj_t * img_logo = lv_image_create(scr);
    lv_image_set_src(img_logo, &logotipo);
    lv_obj_set_style_img_recolor(img_logo, lv_color_hex(0xFFFAF5), 0); // Color blanco roto para el logo
    lv_obj_set_style_img_recolor_opa(img_logo, LV_OPA_COVER, 0); // Aplicar el color completamente
    lv_obj_align(img_logo, LOGO_ALINEACION, LOGO_OFFSET_X, LOGO_OFFSET_Y);

    // Etiqueta de estado BLE
    labelStatus = lv_label_create(scr);
    lv_label_set_text(labelStatus, "BT");
    lv_obj_set_style_text_font(labelStatus, FUENTE_BLE, 0);
    lv_obj_set_style_text_color(labelStatus, COLOR_BLE_DESCONECTADO, 0);
    lv_obj_align(labelStatus, BLE_ALINEACION, BLE_OFFSET_X, BLE_OFFSET_Y);

    // Número de diapositiva
    labelSlide = lv_label_create(scr);
    lv_label_set_text_fmt(labelSlide, "%d", slideNumber);
    lv_obj_set_style_text_font(labelSlide, FUENTE_NUMERO, 0);
    lv_obj_set_style_text_color(labelSlide, COLOR_NUMERO_DIAPOSITIVA, 0);
    lv_obj_align(labelSlide, SLIDE_ALINEACION, SLIDE_OFFSET_X, SLIDE_OFFSET_Y);

    // Temporizador
    labelTimer = lv_label_create(scr);
    lv_label_set_text(labelTimer, "00:00");
    lv_obj_set_style_text_font(labelTimer, FUENTE_TIMER, 0);
    lv_obj_set_style_text_color(labelTimer, COLOR_TEMPORIZADOR, 0);
    lv_obj_align(labelTimer, TIMER_ALINEACION, TIMER_OFFSET_X, TIMER_OFFSET_Y);
}

/**
 * @brief Actualiza el estado de la conexión BLE
 */
void updateBLEStatus() {
    // --- COLORES PARA ESTADOS BLE (FÁCIL DE MODIFICAR) ---
    const lv_color_t COLOR_BLE_CONECTADO = lv_color_hex(0x00FF88);   // Verde claro
    const lv_color_t COLOR_BLE_DESCONECTADO = lv_color_hex(0x9A948F); // Gris
    
    bool isConnected = bleKeyboard.isConnected();
    
    if (isConnected) {
        if (!bleWasConnected) {
            bleWasConnected = true;
            Serial.println("BLE conectado");
        }
        // El texto siempre es "BT", solo cambia el color
        lv_obj_set_style_text_color(labelStatus, COLOR_BLE_CONECTADO, 0);
    } else {
        if (bleWasConnected) {
            bleWasConnected = false;
            Serial.println("BLE desconectado - reintentando...");
            bleKeyboard.end();
            bleKeyboard.begin();
        }
        // El texto siempre es "BT", solo cambia el color
        lv_obj_set_style_text_color(labelStatus, COLOR_BLE_DESCONECTADO, 0);
    }
}

/**
 * @brief Actualiza la visualización del temporizador
 */
void updateTimerDisplay() {
    unsigned long currentTime = timerRunning ? (millis() - timerStartMs) : elapsedMs;
    unsigned int minutes = currentTime / 60000;
    unsigned int seconds = (currentTime / 1000) % 60;
    
    lv_label_set_text_fmt(labelTimer, "%02u:%02u", minutes, seconds);
}

/**
 * @brief Maneja los botones físicos y del expansor
 */
void handlePhysicalButtons() {
    // Leer botones físicos
    uint8_t physTop = digitalRead(BUTTON_TOP);
    
    // Leer botones del expansor
    uint8_t expanderTop = ioexpander.read1(EXPANDER_PAD_TOP);
    uint8_t expanderBottom = ioexpander.read1(EXPANDER_PAD_BOTTOM);
    uint8_t expanderLeft = ioexpander.read1(EXPANDER_PAD_LEFT);
    uint8_t expanderRight = ioexpander.read1(EXPANDER_PAD_RIGHT);
    uint8_t expanderButtonBottom = ioexpander.read1(EXPANDER_BUTTON_BOTTOM);
    
    // Combinar estados (activo si cualquiera está presionado)
    uint8_t top = (physTop == LOW || expanderTop == LOW) ? LOW : HIGH;
    uint8_t bottom = expanderBottom == LOW || expanderButtonBottom == LOW ? LOW : HIGH;
    uint8_t left = expanderLeft;
    uint8_t right = expanderRight;
    
    // Detectar flancos de bajada para avanzar/retroceder diapositivas
    if (lastTop == HIGH && top == LOW) {
        nextSlide();
    }
    
    if (lastBottom == HIGH && bottom == LOW) {
        prevSlide();
    }
    
    // Control del temporizador con botones laterales
    if (lastLeft == HIGH && left == LOW) {
        toggleTimer();
    }
    
    if (lastRight == HIGH && right == LOW) {
        resetTimer();
    }
    
    // Guardar estados anteriores
    lastTop = top;
    lastBottom = bottom;
    lastLeft = left;
    lastRight = right;
}

/**
 * @brief Maneja los eventos de botones táctiles (placeholder)
 */
void handleTouchButtons() {
    // Los eventos de touch se manejan mediante callbacks de LVGL
    // Esta función está disponible para futuras expansiones
}

/**
 * @brief Avanza a la siguiente diapositiva
 */
void nextSlide() {
    if (bleKeyboard.isConnected()) {
        bleKeyboard.write(KEY_RIGHT_ARROW);
        Serial.println("Enviando: Flecha derecha");
    }
    
    slideNumber++;
    lv_label_set_text_fmt(labelSlide, "%d", slideNumber);
    
    // Efecto visual de transición
    lv_obj_set_style_opa(labelSlide, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    delay(50);
    lv_obj_set_style_opa(labelSlide, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    Serial.printf("Diapositiva actual: %d\n", slideNumber);
}

/**
 * @brief Retrocede a la diapositiva anterior
 */
void prevSlide() {
    if (bleKeyboard.isConnected()) {
        bleKeyboard.write(KEY_LEFT_ARROW);
        Serial.println("Enviando: Flecha izquierda");
    }
    
    if (slideNumber > 1) {
        slideNumber--;
    }
    lv_label_set_text_fmt(labelSlide, "%d", slideNumber);
    
    // Efecto visual de transición
    lv_obj_set_style_opa(labelSlide, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    delay(50);
    lv_obj_set_style_opa(labelSlide, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    Serial.printf("Diapositiva actual: %d\n", slideNumber);
}

/**
 * @brief Alterna el estado del temporizador (play/pause)
 */
void toggleTimer() {
    if (!timerRunning) {
        timerRunning = true;
        timerStartMs = millis() - elapsedMs;
        Serial.println("Temporizador iniciado");
    } else {
        timerRunning = false;
        elapsedMs = millis() - timerStartMs;
        Serial.println("Temporizador pausado");
    }
}

/**
 * @brief Reinicia el temporizador
 */
void resetTimer() {
    timerRunning = false;
    elapsedMs = 0;
    lv_label_set_text(labelTimer, "00:00");
    Serial.println("Temporizador reiniciado");
}