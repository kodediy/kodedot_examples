/*
 * AirMouse Firmware (Focused Build)
 * ---------------------------------
 * Control del cursor BLE HID usando la IMU (LSM6DSOX + LIS2MDL) del Kode Dot.
 * Código reducido para máxima respuesta (~100-125 Hz) y mínima latencia.
 *
 * Controles:
 *  - Publica como: AirMouseDemo (BLE HID Mouse)
 *  - Tras la conexión espera ~600 ms y comienza el control.
 *  - Pulsación corta botón TOP: recalibra (centra yaw / pitch / roll actuales = reposo).
 *
 * Mapeo:
 *  - Yaw (giro horizontal) -> movimiento X
 *  - Pitch (inclinación adelante/atrás) -> movimiento Y (invertido: adelante = subir cursor)
 *  - Mezcla opcional de Roll en X para matiz fino
 *
 * Fusión:
 *  - Complementary filter: gyro integra; accel y mag corrigen deriva.
 *  - Heading compensado por inclinación para yaw.
 */

// ----- Includes mínimos -----
#define DIAG_TIMING 1
#include <Arduino.h>
#include <lvgl.h>
#include <kodedot/display_manager.h>
#include <kodedot/pin_config.h>
#include <Wire.h>
#include <Adafruit_LSM6DSOX.h>
#include <Adafruit_LIS2MDL.h>
#include <BleMouseCompat.h>
#include <TCA9555.h>

// ----- Display / UI minimal -----
extern const lv_image_dsc_t cursor_idle;
extern const lv_image_dsc_t cursor_click;
extern const lv_image_dsc_t cursor_disabled;
DisplayManager display;
static lv_obj_t* img_cursor; // imagen grande central

static lv_style_t st_bg, st_txt, st_hint;
static void uiInitStyles(){
    lv_style_init(&st_bg);   lv_style_set_bg_color(&st_bg, lv_color_hex(0x000000));
    lv_style_init(&st_txt);  lv_style_set_text_color(&st_txt, lv_color_hex(0xFFFFFF)); lv_style_set_text_font(&st_txt, &lv_font_montserrat_18);
    lv_style_init(&st_hint); lv_style_set_text_color(&st_hint, lv_color_hex(0x888888)); lv_style_set_text_font(&st_hint, &lv_font_montserrat_14);
}
static bool touchDown=false; // estado táctil actual
static bool touchPressedSent=false; // si ya enviamos press BLE por touch
static void cursorTouchEvent(lv_event_t* e){
    lv_event_code_t code = lv_event_get_code(e);
    if(code==LV_EVENT_PRESSED){ touchDown=true; }
    else if(code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST){ touchDown=false; }
}
static void uiCreate(){
    lv_obj_t* scr = lv_scr_act();
    lv_obj_add_style(scr, &st_bg, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    img_cursor = lv_image_create(scr);
    lv_image_set_src(img_cursor, &cursor_idle);
    lv_obj_center(img_cursor);
    lv_obj_add_flag(img_cursor, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img_cursor, cursorTouchEvent, LV_EVENT_ALL, NULL);
}

// ----- BLE Mouse -----
static BleMouseCompat bleMouse("AirMouseDemo", "Kode", 100);
static bool ble_connected=false; static uint32_t ble_connect_ms=0;

// ----- IMU -----
static Adafruit_LSM6DSOX imu; static Adafruit_LIS2MDL mag(12345);
static TCA9555 ioexp(IOEXP_I2C_ADDR);
// Cache estado botón inferior para evitar I2C continuo
static bool cachedBottom=false; static uint32_t lastBottomPoll=0;

// ----- Fusión orientación -----
static float f_roll=0, f_pitch=0, f_yaw=0;          // filtrados
static float base_roll=0, base_pitch=0, base_yaw=0;  // baseline calibrada
static bool calibrated=false;

// Parámetros
static const float ALPHA_RP=0.85f;   // menor lag (antes 0.94)
static const float ALPHA_YAW=0.97f;  // gyro peso yaw
static const float DEADZONE_DEG=1.0f; // ampliado (antes 0.5) para mayor estabilidad en pequeños grados
static const float MIX_ROLL_X=0.12f;
static const float SENS_YAW_X=1.1f;  // px/deg
static const float SENS_PITCH_Y=1.1f;// px/deg

// Sensibilidad reducida para pruebas
static const float SENS_YAW_X_LOW=0.5f;
static const float SENS_PITCH_Y_LOW=0.5f;
static const float NONLIN_EXP=1.18f; // un poco menos para que arranque más rápido
static const int8_t MAX_STEP=40; // aumentar límite BLE frame
static const uint32_t FUSION_INTERVAL_MS=3; // subimos frecuencia lógica (antes 5)
static const uint32_t MOUSE_INTERVAL_MS=3;  // alineado

// ----- Mapeo SIMPLE relativo: Yaw -> X, Roll -> Y -----
static float SENS_YAW = 0.9f;   // sensibilidad base px / grado (post curva)
static float SENS_ROLL = 0.9f;  // sensibilidad base px / grado (post curva)
static bool INVERT_Y = false;   // invertir eje vertical si se siente al revés
// Suavizado adaptativo: se ajusta en runtime (LOW_SMOOTH cuando movimiento grande)
static float LOW_SMOOTH = 0.10f;
static float HIGH_SMOOTH = 0.28f;
static float SMOOTH = HIGH_SMOOTH; // valor inicial
// Ganancia por velocidad angular (deg/s -> px extra por frame)
static float RATE_GAIN_YAW = 0.0f;
static float RATE_GAIN_ROLL = 0.0f;
// Umbrales para cambiar suavizado
static float FAST_MOVE_THRESHOLD = 6.0f; // grados (post deadzone) para considerar movimiento rápido
static float acc_dx=0, acc_dy=0; // acumuladores filtrados
// ----- Vector de movimiento integrado -----
// (Desactivado) vector integrado; pasamos a modo RATE directo
static bool VECTOR_MODE = false;
// Modo RATE + leve inercia/acceleración (sensibilidad algo mayor)
static const float RATE_GAIN_YAW_PX_PER_DPS = 0.24f;  // antes 0.18 -> más velocidad base
static const float RATE_GAIN_ROLL_PX_PER_DPS = 0.24f;
static const float RATE_DEADZONE_DPS = 4.4f;          // zona de quietud ampliada (antes 2.2)
static const float RATE_MAX_STEP = 34.0f;             // permitir ráfagas mayores
// Aceleración suave: factor adicional según magnitud de la velocidad angular
static const float ACCEL_FACTOR = 0.0f;               // sin aceleración
static const float ACCEL_REF_DPS = 160.0f;            // (inactivo)
// Inercia leve: porcentaje de la velocidad previa que persiste (0 = sin inercia)
static const float INERTIA_FACTOR = 0.0f;             // sin inercia
static const float INERTIA_CUTOFF = 0.20f;            // px/frame mínimo para seguir reteniendo (antes 0.35)
static const float MIN_FRAME_PIX = 0.8f;              // un poco menor para suavizar arranque
static float vel_x=0.0f, vel_y=0.0f;                  // estado de velocidad
static float frac_x=0.0f, frac_y=0.0f;                // restos fraccionarios
// ---- Parámetros avanzados de estabilización (sin magnetómetro) ----
// Detección de reposo para estimar bias del gyro y mitigar deriva yaw
static const float GYRO_STATIONARY_DPS = 1.2f;          // umbral dps para considerar eje en reposo
static const float ACC_NORM_G = 9.81f;                  // 1 g
static const float ACC_NORM_TOL = 1.2f;                 // tolerancia absoluta (m/s^2) sobre 1g
static const uint32_t STATIONARY_HOLD_MS = 280;         // tiempo mínimo quieto antes de integrar bias
static const float BIAS_ALPHA_Z = 0.0025f;              // rapidez de convergencia bias Z (lenta para estabilidad)
static const float DRIFT_RECENTER_ALPHA = 0.18f;        // factor de recentrado suave cuando quieto (relativo a dt)
static const float DRIFT_RECENTER_LIMIT_DEG = 4.0f;     // sólo corrige drift pequeño
static float bias_gz_dps = 0.0f;                        // bias estimado eje Z (deg/s)
static uint32_t stationaryStartMs = 0;                  // marca inicio de periodo quieto
static bool wasStationary = false;
static float last_acc_norm = ACC_NORM_G;                // última norma de aceleración
// Compresión suave para grandes giros (suaviza extremos)
static float rateSoftLimit(float v){
    // tanh normaliza manteniendo linealidad cerca de 0
    const float SCALE = 0.04f; // para ~25 -> tanh(1)=0.76 => escalado luego
    return tanhf(v * SCALE) * (RATE_MAX_STEP);
}

static uint32_t lastFusion=0, lastMouse=0, lastAnglesUi=0;

// ====== Fusión avanzada (Madgwick) ======
enum FusionMode { FUSION_COMPLEMENTARY=0, FUSION_MADGWICK=1 };
static FusionMode FUSION_MODE = FUSION_COMPLEMENTARY; // fijamos complementario para simplificar mapeo
// Estado cuaternión (Madgwick)
static float q0=1.0f, q1=0.0f, q2=0.0f, q3=0.0f; // (no usado en modo simple)
// Ganancia Madgwick (responsividad vs estabilidad). 0.02–0.15 típico.
static float MADGWICK_BETA = 0.06f;
// Integración directa de yaw a partir del gyro Z (más estable respecto al mapeo esperado yaw->X)
static float yaw_gyro=0.0f;
static float last_gx_deg=0, last_gy_deg=0, last_gz_deg=0; // tasas últimas (deg/s)
static void setQuatFromEuler(float rollDeg,float pitchDeg,float yawDeg){
    float r=rollDeg*DEG_TO_RAD*0.5f;
    float p=pitchDeg*DEG_TO_RAD*0.5f;
    float y=yawDeg*DEG_TO_RAD*0.5f;
    float cr=cosf(r), sr=sinf(r);
    float cp=cosf(p), sp=sinf(p);
    float cy=cosf(y), sy=sinf(y);
    q0 = cy*cp*cr + sy*sp*sr;
    q1 = cy*cp*sr - sy*sp*cr;
    q2 = cy*sp*cr + sy*cp*sr;
    q3 = sy*cp*cr - cy*sp*sr;
}
static void toggleFusionMode(){
    if(FUSION_MODE==FUSION_MADGWICK){
        FUSION_MODE = FUSION_COMPLEMENTARY;
    } else {
        // Inicializar cuaternión desde los ángulos actuales para evitar salto
        setQuatFromEuler(f_roll, f_pitch, f_yaw);
        FUSION_MODE = FUSION_MADGWICK;
    }
    Serial.printf("[FUSION] Modo ahora: %s\n", FUSION_MODE==FUSION_MADGWICK?"Madgwick":"Complementary");
}

static void quatToEuler(float &rollDeg, float &pitchDeg, float &yawDeg){
    // Referencia: Aerospace sequence (roll X, pitch Y, yaw Z)
    float ysqr = q2*q2;
    float t0 = +2.0f * (q0*q1 + q2*q3);
    float t1 = +1.0f - 2.0f * (q1*q1 + ysqr);
    float roll = atan2f(t0, t1);
    float t2 = +2.0f * (q0*q2 - q3*q1);
    t2 = t2> 1.0f ? 1.0f : t2;
    t2 = t2<-1.0f ?-1.0f : t2;
    float pitch = asinf(t2);
    float t3 = +2.0f * (q0*q3 + q1*q2);
    float t4 = +1.0f - 2.0f * (ysqr + q3*q3);
    float yaw = atan2f(t3, t4);
    rollDeg  = roll * RAD_TO_DEG;
    pitchDeg = pitch* RAD_TO_DEG;
    yawDeg   = yaw  * RAD_TO_DEG;
    if(yawDeg<0) yawDeg+=360.0f;
}

static float invSqrt(float x){ return 1.0f/sqrtf(x); }

static void madgwickUpdate(float gx, float gy, float gz, float ax, float ay, float az, float mx, float my, float mz, float dt){
    // Basado en algoritmo público de Madgwick (simplificado)
    float q0q0 = q0*q0, q1q1=q1*q1, q2q2=q2*q2, q3q3=q3*q3;
    // Normalizar acelerómetro
    float norm = ax*ax+ay*ay+az*az; if(norm<1e-9f) return; norm = invSqrt(norm); ax*=norm; ay*=norm; az*=norm;
    bool haveMag = (fabsf(mx)+fabsf(my)+fabsf(mz))>0.01f;
    if(haveMag){
        norm = mx*mx+my*my+mz*mz; if(norm<1e-9f) haveMag=false; else { norm=invSqrt(norm); mx*=norm; my*=norm; mz*=norm; }
    }
    float _2q0=2.0f*q0, _2q1=2.0f*q1, _2q2=2.0f*q2, _2q3=2.0f*q3;
    float _4q0=4.0f*q0, _4q1=4.0f*q1, _4q2=4.0f*q2; // _4q3 not needed
    float _8q1=8.0f*q1, _8q2=8.0f*q2;
    float s0,s1,s2,s3;
    if(haveMag){
        float q0q1 = q0*q1, q0q2=q0*q2, q0q3=q0*q3, q1q2=q1*q2, q1q3=q1*q3, q2q3=q2*q3;
        float hx = 2.0f*mx*(0.5f - q2q2 - q3q3) + 2.0f*my*(q1q2 - q0q3) + 2.0f*mz*(q1q3 + q0q2);
        float hy = 2.0f*mx*(q1q2 + q0q3) + 2.0f*my*(0.5f - q1q1 - q3q3) + 2.0f*mz*(q2q3 - q0q1);
        float _2bx = sqrtf(hx*hx + hy*hy);
        float _2bz = 2.0f*mx*(q1q3 - q0q2) + 2.0f*my*(q2q3 + q0q1) + 2.0f*mz*(0.5f - q1q1 - q2q2);
        float _4bx = 2.0f*_2bx;
        float _4bz = 2.0f*_2bz;
        s0 = -_2q2*(2.0f*(q1q3 - q0q2) - 2.0f*ax) + _2q1*(2.0f*(q0q1 + q2q3) - 2.0f*ay)
             - _2bz*q2*(_2bx*(0.5f - q2q2 - q3q3) + _2bz*(q1q3 - q0q2) - mx)
             + (-_2bx*q3 + _2bz*q1)*(_2bx*(q1q2 - q0q3) + _2bz*(q0q1 + q2q3) - my)
             + _2bx*q2*(_2bx*(q0q2 + q1q3) + _2bz*(0.5f - q1q1 - q2q2) - mz);
        s1 = _2q3*(2.0f*(q1q3 - q0q2) - 2.0f*ax) + _2q0*(2.0f*(q0q1 + q2q3) - 2.0f*ay)
             - 4.0f*q1*(1 - 2.0f*(q1q1 + q2q2) - 2.0f*az)
             + (_2bz*q3 - _2bx*q1)*(_2bx*(0.5f - q2q2 - q3q3) + _2bz*(q1q3 - q0q2) - mx)
             + (_2bx*q2 + _2bz*q0)*(_2bx*(q1q2 - q0q3) + _2bz*(q0q1 + q2q3) - my)
             + (_2bx*q3 - _4bz*q1)*(_2bx*(q0q2 + q1q3) + _2bz*(0.5f - q1q1 - q2q2) - mz);
        s2 = -_2q0*(2.0f*(q1q3 - q0q2) - 2.0f*ax) + _2q3*(2.0f*(q0q1 + q2q3) - 2.0f*ay)
             - 4.0f*q2*(1 - 2.0f*(q1q1 + q2q2) - 2.0f*az)
             + (-_4bx*q2 - _2bz*q0)*(_2bx*(0.5f - q2q2 - q3q3) + _2bz*(q1q3 - q0q2) - mx)
             + (_2bx*q1 + _2bz*q3)*(_2bx*(q1q2 - q0q3) + _2bz*(q0q1 + q2q3) - my)
             + (_2bx*q0 - _4bz*q2)*(_2bx*(q0q2 + q1q3) + _2bz*(0.5f - q1q1 - q2q2) - mz);
        s3 = _2q1*(2.0f*(q1q3 - q0q2) - 2.0f*ax) + _2q2*(2.0f*(q0q1 + q2q3) - 2.0f*ay)
             + (-_4bx*q3 + _2bz*q1)*(_2bx*(0.5f - q2q2 - q3q3) + _2bz*(q1q3 - q0q2) - mx)
             + (-_2bx*q0 + _2bz*q2)*(_2bx*(q1q2 - q0q3) + _2bz*(q0q1 + q2q3) - my)
             + (_2bx*q1)*(_2bx*(q0q2 + q1q3) + _2bz*(0.5f - q1q1 - q2q2) - mz);
    } else {
        // 6DOF variante
        s0 = _4q0*q2q2 + _2q2*az + _4q0*q1q1 - _2q1*ay;
        s1 = _4q1*q3q3 - _2q3*ax + 4.0f*q0q0*q1 - _2q0*ay - _4q1 + _8q1*q1q1 + _8q1*q2q2 + _4q1*az;
        s2 = 4.0f*q0q0*q2 + _2q0*ax + _4q2*q3q3 - _2q3*ay - _4q2 + _8q2*q1q1 + _8q2*q2q2 + _4q2*az;
        s3 = 4.0f*q1q1*q3 - _2q1*ax + 4.0f*q2q2*q3 - _2q2*ay;
    }
    norm = invSqrt(s0*s0 + s1*s1 + s2*s2 + s3*s3);
    s0*=norm; s1*=norm; s2*=norm; s3*=norm;
    // Derivada cuaternión por gyros
    float qDot0 = 0.5f * (-q1*gx - q2*gy - q3*gz) - MADGWICK_BETA*s0;
    float qDot1 = 0.5f * ( q0*gx + q2*gz - q3*gy) - MADGWICK_BETA*s1;
    float qDot2 = 0.5f * ( q0*gy - q1*gz + q3*gx) - MADGWICK_BETA*s2;
    float qDot3 = 0.5f * ( q0*gz + q1*gy - q2*gx) - MADGWICK_BETA*s3;
    // Integrar
    q0 += qDot0*dt; q1 += qDot1*dt; q2 += qDot2*dt; q3 += qDot3*dt;
    norm = invSqrt(q0*q0 + q1*q1 + q2*q2 + q3*q3); q0*=norm; q1*=norm; q2*=norm; q3*=norm;
}

static inline bool topPressed(){ return digitalRead(BUTTON_TOP)==LOW; }
static void pollBottom(){
    uint32_t now=millis();
    if(now - lastBottomPoll < 12) return; // ~83 Hz
    lastBottomPoll = now;
    if(ioexp.isConnected()){
        int v = ioexp.read1(BUTTON_BOTTOM);
        if(v != TCA9555_INVALID_READ){
            cachedBottom = (v==LOW);
        }
    }
}
static inline bool bottomPressed(){ return cachedBottom; }
static void calibrate(){ base_roll=f_roll; base_pitch=f_pitch; base_yaw=f_yaw; calibrated=true; Serial.printf("[CAL] r=%.2f p=%.2f y=%.2f\n",base_roll,base_pitch,base_yaw);} 

static float tiltHeading(float mx,float my,float mz,float rRad,float pRad){ float cr=cosf(rRad),sr=sinf(rRad),cp=cosf(pRad),sp=sinf(pRad); float mx2=mx*cp + mz*sp; float my2=mx*sr*sp + my*cr - mz*sr*cp; float h=atan2f(-my2,mx2)*180.0f/PI; if(h<0) h+=360.0f; return h; }

static void initImu(){
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL);
    Wire.setClock(400000); // I2C fast mode
    bool ok1=imu.begin_I2C();
    if(ok1){
        imu.setAccelRange(LSM6DS_ACCEL_RANGE_4_G);
        imu.setGyroRange(LSM6DS_GYRO_RANGE_500_DPS);
        imu.setAccelDataRate(LSM6DS_RATE_208_HZ);
        imu.setGyroDataRate(LSM6DS_RATE_208_HZ);
    }
    bool ok2=mag.begin();
    Serial.printf("IMU:%s MAG:%s\n",ok1?"OK":"FAIL", ok2?"OK":"FAIL");
}
static void initIOExp(){
    if(!ioexp.begin(INPUT)){
        Serial.println("IOEXP: FAIL");
        return;
    }
    ioexp.pinMode1(BUTTON_BOTTOM, INPUT);
    Serial.println("IOEXP: OK");
}

static uint32_t lastMagMs = 0; // lectura mag desacoplada (más lenta)
static void updateFusion(){
    uint32_t now=millis();
    if(now-lastFusion<FUSION_INTERVAL_MS) return;
    float dt=(now-lastFusion)/1000.0f;
    if(lastFusion==0){lastFusion=now; return;}
    lastFusion=now;
    sensors_event_t a,g,t;
    if(!imu.getEvent(&a,&g,&t)) return; // esta llamada ya obtiene accel+gyro (mag separado)
    float gx=g.gyro.x, gy=g.gyro.y, gz=g.gyro.z; // rad/s
    // Convertimos a deg/s tempranamente
    float gx_dps = gx*RAD_TO_DEG;
    float gy_dps = gy*RAD_TO_DEG;
    float gz_dps = gz*RAD_TO_DEG;
    // Detección de reposo: bajas velocidades angulares y aceleración ~1g
    float accNorm = sqrtf(a.acceleration.x*a.acceleration.x + a.acceleration.y*a.acceleration.y + a.acceleration.z*a.acceleration.z);
    last_acc_norm = accNorm;
    bool lowGyro = fabsf(gx_dps)<GYRO_STATIONARY_DPS && fabsf(gy_dps)<GYRO_STATIONARY_DPS && fabsf(gz_dps)<GYRO_STATIONARY_DPS;
    bool accOk = fabsf(accNorm - ACC_NORM_G) < ACC_NORM_TOL; // no grandes movimientos lineales
    bool stationary = (lowGyro && accOk);
    if(stationary){
        if(!wasStationary) stationaryStartMs = now;
        else if(now - stationaryStartMs > STATIONARY_HOLD_MS){
            // Actualizar bias Z lentamente
            bias_gz_dps += BIAS_ALPHA_Z * (gz_dps - bias_gz_dps);
        }
    }
    wasStationary = stationary;
    // Corregir yaw rate con bias estimado
    float gz_corr_dps = gz_dps - bias_gz_dps;
    last_gx_deg = gx_dps;
    last_gy_deg = gy_dps;
    last_gz_deg = gz_corr_dps; // ya corregido para uso de movimiento
    float ax=a.acceleration.x, ay=a.acceleration.y, az=a.acceleration.z;
    bool haveMag = false; sensors_event_t m; // lectura mag sólo cada 80ms para no bloquear
    if(now - lastMagMs > 80){
        if(mag.getEvent(&m)) haveMag = true;
        lastMagMs = now;
    }
    // Modo simplificado: usamos integración gyro para yaw exclusivamente (eje Z) y filtrado complementario solo para roll.
    float rollRad=atan2f(ay,az);
    float pitchRad=atan2f(-ax, sqrtf(ay*ay+az*az));
    float accRoll=rollRad*RAD_TO_DEG; // Sólo necesitamos roll del accel
    // Integrar gyro
    f_roll += gx*RAD_TO_DEG*dt; // temporal (antes de mezcla)
    yaw_gyro += gz_corr_dps*dt; // yaw corregida bias
    // Normalizar yaw a [-180,180]
    while(yaw_gyro>180) yaw_gyro-=360; while(yaw_gyro<-180) yaw_gyro+=360;
    // Recentrado anti-deriva suave sólo cuando está quieto y deriva pequeña
    if(stationary){
        float dy = yaw_gyro - base_yaw; // desplazamiento respecto a baseline
        if(fabsf(dy) < DRIFT_RECENTER_LIMIT_DEG){
            float gamma = DRIFT_RECENTER_ALPHA * dt; if(gamma>0.25f) gamma=0.25f; // segura
            yaw_gyro -= dy * gamma; // acerca a baseline
        }
    }
    // Complementary para roll
    f_roll = ALPHA_RP*f_roll + (1-ALPHA_RP)*accRoll;
    f_yaw = yaw_gyro; // usamos yaw integrada
    // Pitch dejamos disponible si se quiere mostrar
    float accPitch = pitchRad*RAD_TO_DEG;
    f_pitch = accPitch; // sin uso actual
    if(!calibrated){ base_roll=f_roll; base_pitch=f_pitch; base_yaw=f_yaw; calibrated=true; }
    // Eliminado texto de ángulos para interfaz limpia
    // Botones ahora gestionados en updateMouse (click / freeze)
}


static float dz(float v){
    if(fabsf(v)<DEADZONE_DEG) return 0;
    return v - copysignf(DEADZONE_DEG,v);
}

static float curve(float v){
    // Curva adaptativa: lineal en rangos pequeños para respuesta inmediata, exponencial fuera
    if(v==0) return 0;
    float s = v<0?-1:1; v=fabsf(v);
    if(v < 5.0f) { // zona fina
        return s*v; // lineal
    }
    return s*powf(v,NONLIN_EXP);
}

static void updateMouse(){
    uint32_t now=millis();
    bool conn=bleMouse.isConnected();
    if(conn && !ble_connected){ ble_connected=true; ble_connect_ms=now; base_yaw=f_yaw; base_roll=f_roll; calibrated=true; Serial.println("[BLE] Connected y calibrado"); }
    else if(!conn && ble_connected){ ble_connected=false; Serial.println("[BLE] Disconnected"); return; }
    if(!conn) return;
    if(now-ble_connect_ms<600) return;
    // Gestión botones (click y freeze)
    pollBottom();
    static bool lastTop=false, lastBottom=false;
    bool topRaw = topPressed();
    bool bottom = bottomPressed();
    // Debounce top (simple)
    static uint32_t topStateChangeMs=0; static bool topStable=false; static bool topPrevRaw=false;
    if(topRaw != topPrevRaw){ topPrevRaw=topRaw; topStateChangeMs=now; }
    if(now - topStateChangeMs > 25) topStable = topRaw; // estable tras 25ms
    bool top = topStable;
    static uint32_t topPressMs=0;
    static uint32_t clickFreezeUntil=0; // ventana de estabilización al pulsar
    static float freezeYawBase=0, freezeRollBase=0;    // baseline congelada durante click
    // Click handling with separate press/release to avoid blocking BLE stack
    static bool pendingPress=false; static bool pendingRelease=false; static uint32_t releaseAt=0; static uint32_t lastClickMs=0; const uint32_t CLICK_GAP=80; // mínimo entre clicks
    if(top && !lastTop){ topPressMs=now; clickFreezeUntil=now+55; freezeYawBase=f_yaw; freezeRollBase=f_roll; }
    if(!top && lastTop){
        uint32_t dur = now - topPressMs;
        if(dur < 600){
            if(now - lastClickMs > CLICK_GAP){
                pendingPress=true; pendingRelease=false; lastClickMs=now; // schedule press now, release shortly
            }
        } else {
            calibrate();
        }
    }
    if(pendingPress){ bleMouse.press(MOUSE_LEFT); pendingPress=false; pendingRelease=true; releaseAt=now+25; }
    if(pendingRelease && now >= releaseAt){ bleMouse.release(MOUSE_LEFT); pendingRelease=false; }
    // Freeze mientras bottom pulsado
    if(!bottom && lastBottom){ base_yaw=f_yaw; base_roll=f_roll; vel_x=vel_y=0; frac_x=frac_y=0; }
    lastTop=top; lastBottom=bottom;
    if(now-lastMouse<MOUSE_INTERVAL_MS) return;
    lastMouse=now;
    bool freezeAll = false;
    if(bottom){ // congelado total
        if(touchPressedSent){ bleMouse.release(MOUSE_LEFT); touchPressedSent=false; }
        freezeAll = true;
    }
    // Congelar también durante pulsación corta (reducción de micro-movimiento involuntario) y ventana post-pulsación
    bool freezeClickWindow = false;
    if(!freezeAll && (top || now < clickFreezeUntil)){
        base_yaw = freezeYawBase; base_roll = freezeRollBase; vel_x=vel_y=0; frac_x=frac_y=0; freezeClickWindow = true;
    }
    if(!calibrated){ calibrate(); freezeAll = true; }
    float dYaw=0, dRoll=0; // ya no usados directamente para rate pero mantenidos por si se reintroduce
    if(!freezeAll && !freezeClickWindow){
        dYaw = f_yaw - base_yaw;
        while(dYaw > 180) dYaw -= 360;
        while(dYaw < -180) dYaw += 360;
        dRoll = f_roll - base_roll;
        while(dRoll > 180) dRoll -= 360;
        while(dRoll < -180) dRoll += 360;
        dYaw = curve(dz(dYaw));
        dRoll = curve(dz(dRoll));
    }

    // Jitter suppression: escalar inputs muy pequeños para reducir oscilación, mantener respuesta en movimientos claros
    int8_t dx=0, dy=0;
    if(!freezeAll && !freezeClickWindow){
    const float JITTER_RATE_THRESH_DPS = 5.0f;   // antes 8.0f
    const float JITTER_EXP = 1.2f;               // antes 1.4f
        float rateYaw = last_gz_deg;
        float rateRoll = last_gx_deg;
        if(fabsf(rateYaw) < RATE_DEADZONE_DPS) rateYaw = 0;
        if(fabsf(rateRoll) < RATE_DEADZONE_DPS) rateRoll = 0;
        auto compressSmall = [&](float r){ float s=(r<0?-1:1); r=fabsf(r); if(r < JITTER_RATE_THRESH_DPS){ float n=powf(r/JITTER_RATE_THRESH_DPS, JITTER_EXP)*JITTER_RATE_THRESH_DPS; return s*n;} return s*r; };
        rateYaw = compressSmall(rateYaw);
        rateRoll = compressSmall(rateRoll);
    float fx = rateYaw * RATE_GAIN_YAW_PX_PER_DPS;
    float fy = rateRoll * RATE_GAIN_ROLL_PX_PER_DPS;
        // Aceleración desactivada: respuesta estrictamente lineal al rate filtrado
    if(fx>0) fx = rateSoftLimit(fx); else fx = -rateSoftLimit(-fx);
    if(fy>0) fy = rateSoftLimit(fy); else fy = -rateSoftLimit(-fy);
    if(fx!=0 && fabsf(fx) < MIN_FRAME_PIX) fx = copysignf(MIN_FRAME_PIX, fx);
    if(fy!=0 && fabsf(fy) < MIN_FRAME_PIX) fy = copysignf(MIN_FRAME_PIX, fy);
        if(INVERT_Y) fy = -fy;
    // Sin inercia: tomamos directamente la contribución actual
    vel_x = fx;
    vel_y = fy;
        if(fabsf(vel_x) < INERTIA_CUTOFF) vel_x=0;
        if(fabsf(vel_y) < INERTIA_CUTOFF) vel_y=0;
        if(vel_x > RATE_MAX_STEP) vel_x = RATE_MAX_STEP; else if(vel_x < -RATE_MAX_STEP) vel_x = -RATE_MAX_STEP;
        if(vel_y > RATE_MAX_STEP) vel_y = RATE_MAX_STEP; else if(vel_y < -RATE_MAX_STEP) vel_y = -RATE_MAX_STEP;
        frac_x += vel_x; frac_y += vel_y;
        float send_x = frac_x; float send_y = frac_y;
        if(send_x > MAX_STEP) send_x = MAX_STEP; else if(send_x < -MAX_STEP) send_x = -MAX_STEP;
        if(send_y > MAX_STEP) send_y = MAX_STEP; else if(send_y < -MAX_STEP) send_y = -MAX_STEP;
    dx = (int8_t)lrintf(send_x); dy = (int8_t)lrintf(send_y);
        if(dx||dy){ bleMouse.move(dx,dy,0); frac_x -= dx; frac_y -= dy; }
    }
    // Gestión de click por touch (mantener presionado = botón abajo)
    if(touchDown && !touchPressedSent && !bottom){ bleMouse.press(MOUSE_LEFT); touchPressedSent=true; }
    if(!touchDown && touchPressedSent){ bleMouse.release(MOUSE_LEFT); touchPressedSent=false; }
    // Selección de imagen según prioridad: bottom(disabled) > click(top/touch/pending) > idle
    const lv_image_dsc_t* desired = &cursor_idle;
    if(bottom) desired = &cursor_disabled;
    else if(top || touchPressedSent || pendingPress || pendingRelease) desired = &cursor_click;
    if(img_cursor){ static const lv_image_dsc_t* lastImg=nullptr; if(desired!=lastImg){ lv_image_set_src(img_cursor, desired); lastImg=desired; } }
}

static uint32_t loopMaxUs=0, loopLastPrint=0;
static uint32_t fusMaxUs=0, mouseMaxUs=0;
void setup(){ Serial.begin(115200); Serial.println("AirMouse start"); if(!display.init()){ Serial.println("Display fail"); while(true) delay(1000);} uiInitStyles(); uiCreate(); pinMode(BUTTON_TOP, INPUT_PULLUP); initIOExp(); initImu(); bleMouse.begin(); }
void loop(){
    uint32_t loopStart = micros();
    uint32_t t0 = micros(); updateFusion(); uint32_t t1=micros(); if(t1-t0>fusMaxUs) fusMaxUs=t1-t0;
    updateMouse(); uint32_t t2=micros(); if(t2-t1>mouseMaxUs) mouseMaxUs=t2-t1;
    static uint32_t loopCounter=0;
    if((loopCounter++ & 0x01)==0) display.update(); // mitad de frecuencia -> menos jitter
    uint32_t dur = micros()-loopStart; if(dur>loopMaxUs) loopMaxUs=dur;
    if(millis() - loopLastPrint > 1500){
        loopLastPrint = millis();
        Serial.printf("[TIM] loopMax=%lu us fusionMax=%lu us mouseMax=%lu us vel(%.2f,%.2f) frac(%.2f,%.2f)\n", (unsigned long)loopMaxUs,(unsigned long)fusMaxUs,(unsigned long)mouseMaxUs, vel_x, vel_y, frac_x, frac_y);
        loopMaxUs=fusMaxUs=mouseMaxUs=0;
    }
    delayMicroseconds(300); // menor latencia que delay(1)
}