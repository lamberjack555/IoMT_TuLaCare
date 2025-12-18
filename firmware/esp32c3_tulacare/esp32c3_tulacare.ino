#include <Wire.h>
#include <MAX30105.h>
#include <spo2_algorithm.h>
#include <math.h>

// ==================== WIFI + MQTT (CẤU HÌNH CLOUD) ====================
#include <WiFi.h>
#include <WiFiClientSecure.h> // <<< QUAN TRỌNG: Thư viện bảo mật cho SSL
#include <PubSubClient.h>

// ---- Điền thông tin mạng ----
const char* WIFI_SSID  = "P502 K8";
const char* WIFI_PASS  = "123456789";

// ---- Điền thông tin HiveMQ Cloud ----
const char* MQTT_HOST  = "7ea4531d69e74f51b70c14213c7980e4.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORT = 8883; // Cổng SSL
const char* MQTT_USER  = "esp32c3_tunglam";
const char* MQTT_PASS  = "Tunglam.03";
const char* DEV_ID     = "dev001";

WiFiClientSecure espClient; // Dùng WiFiClientSecure
PubSubClient mqtt(espClient);

// Các biến MQTT
String topicData, topicEvent, topicCmd;
unsigned long lastMqttAttempt = 0;
const unsigned long MQTT_RETRY_MS = 5000;

// ==================== PINS ====================
#define SDA_PIN 8
#define SCL_PIN 9
#define BUZZER_PIN 3

// ==================== BUZZER ====================
#define BUZZER_FREQ 2731
#define BUZZER_RES  8

// BOOT-MUTE
const unsigned long BOOT_MUTE_MS = 10000UL; 
static bool buzzerLockout = true;
static bool buzzerReady   = false;           
static unsigned long bootMs = 0;

// ==================== MPU6050 ====================
#define MPU_ADDR_68 0x68
#define MPU_ADDR_69 0x69
uint8_t MPU_ADDR = MPU_ADDR_68;

#define REG_SMPLRT_DIV      0x19
#define REG_CONFIG          0x1A
#define REG_GYRO_CONFIG     0x1B
#define REG_ACCEL_CONFIG    0x1C
#define REG_FIFO_EN         0x23
#define REG_INT_PIN_CFG     0x37
#define REG_INT_ENABLE      0x38
#define REG_INT_STATUS      0x3A
#define REG_ACCEL_XOUT_H    0x3B
#define REG_PWR_MGMT_1      0x6B
#define REG_PWR_MGMT_2      0x6C
#define REG_MOT_THR         0x1F
#define REG_MOT_DUR         0x20
#define REG_MOT_DETECT_CTRL 0x69

const float ACC_SCALE = 16384.0f;
const float GYR_SCALE = 131.0f;
float roll_deg = 0, pitch_deg = 0;
const float COMP_ALPHA = 0.98f;

// ==================== FALL DETECTION (v2) ====================
const float THR_FREEFALL_G       = 0.5f;
const float THR_IMPACT_MIN_G     = 1.8f;
const float THR_IMPACT_MAX_G     = 4.5f; 
const float THR_IMPACT_GYRO_G    = 120.0f;
const float THR_LYING_AZ         = 0.4f;
const unsigned long WIN_FREEFALL_MS = 400;
const unsigned long WIN_ANGLE_MS = 500;
const float THR_GYRO_IDLE        = 15.0f;
const float THR_AMAG_VAR         = 0.015f;
const unsigned long REQ_INACT_MS = 2500;
const unsigned long LATCH_MS     = 12000;

const float QUIET_GYRO_THR = 6.0f;
const float QUIET_AMAG_VAR = 0.003f;
const unsigned long ACTIVE_QUIET_BACK_MS = 6000;

const int VAR_WIN = 130;
float aBuf[VAR_WIN]; int aIdx=0, aCount=0; float aSum=0, aSum2=0;

enum FallState { F_IDLE, F_POSSIBLE, F_CONFIRM, F_FALL_LATCH };
FallState fstate = F_IDLE;
unsigned long tImpact=0, tLatch=0, tFreeFall=0;
bool angleOK=false; 
bool hadFreeFall=false;

// Biến lưu lực va chạm để tính cấp độ
float impactForce = 0.0f; 

float cumulativeInactMs = 0;
static unsigned long lastFsmStepMs = 0;
float g_ax = 0, g_ay = 0, g_az = 0;

enum RunMode { IDLE_LP, ACTIVE };
RunMode mode = IDLE_LP;

unsigned long lastUs=0, lastPrintMs=0, quietStartMs=0;
int readFailCount=0;

// ==================== MAX30102 ====================
MAX30105 particleSensor;
bool maxInited = false;

int curLED_RED_mA = 30;
int curLED_IR_mA  = 60;

const unsigned long PPG_PERIOD_MS   = 5000UL;
const unsigned long PPG_WARMUP_MS   = 1500UL;
const unsigned long PPG_MEAS_MIN_MS = 8000UL;
const unsigned long PPG_MEAS_MAX_MS = 15000UL;
unsigned long ppgNextDueMs = 2000UL;
unsigned long ppgPhaseStartMs = 0;

enum PPGState { PPG_IDLE, PPG_WARMUP, PPG_MEAS, PPG_DONE };
PPGState ppgState = PPG_IDLE;

const int PPG_MAX_SAMPLES = 1500;
uint32_t irBuf[PPG_MAX_SAMPLES];
uint32_t redBuf[PPG_MAX_SAMPLES];
int ppgCount = 0;

float hrVec[10]; float spo2Vec[10]; int validCount = 0;

const uint32_t IR_DC_MIN   = 12000;
const float    MOTION_G_THR= 7.0f;
const float    AMAG_VAR_THR= 0.007f;

const uint32_t DC_TARGET_MIN = 80000;
const uint32_t DC_TARGET_MAX = 200000;
const int LED_RED_MIN = 8,  LED_RED_MAX = 50;
const int LED_IR_MIN  = 12, LED_IR_MAX  = 60;

#define SPO2_WIN 100
static uint32_t irWin[SPO2_WIN];
static uint32_t redWin[SPO2_WIN];

// ==================== I2C HELPER ====================
bool i2cWriteByte(uint8_t addr, uint8_t reg, uint8_t data) {
  Wire.beginTransmission(addr); Wire.write(reg); Wire.write(data); 
  return Wire.endTransmission() == 0; 
}

bool i2cReadBytes(uint8_t addr, uint8_t reg, uint8_t *buffer, size_t len) {
  Wire.beginTransmission(addr); Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t bytes = Wire.requestFrom((int)addr, (int)len, true); 
  if (bytes != len) return false;
  for (size_t i = 0; i < len; i++) buffer[i] = Wire.read(); 
  return true;
}

void i2cBusRecovery() {
  Wire.end(); delay(2);
  pinMode(SCL_PIN, OUTPUT); pinMode(SDA_PIN, INPUT_PULLUP);
  for (int i = 0; i < 9; i++) {
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(5); 
    digitalWrite(SCL_PIN, LOW); delayMicroseconds(5); 
  }
  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  Wire.setTimeOut(50); delay(2);
}

uint8_t scanPickMPUAddr() {
  bool found68 = false, found69 = false;
  for (uint8_t addr = 1; addr < 127; addr++) { 
    Wire.beginTransmission(addr); 
    if (Wire.endTransmission() == 0) { 
      if (addr == MPU_ADDR_68) found68 = true; 
      if (addr == MPU_ADDR_69) found69 = true; 
    } 
  }
  if (found68) return MPU_ADDR_68; 
  if (found69) return MPU_ADDR_69; 
  return MPU_ADDR_68;
}

bool setActiveMode100Hz() {
  if (!i2cWriteByte(MPU_ADDR, REG_PWR_MGMT_1, 0x01)) return false;
  if (!i2cWriteByte(MPU_ADDR, REG_PWR_MGMT_2, 0x00)) return false;
  if (!i2cWriteByte(MPU_ADDR, REG_CONFIG, 0x03)) return false;
  if (!i2cWriteByte(MPU_ADDR, REG_GYRO_CONFIG, 0x00)) return false;
  if (!i2cWriteByte(MPU_ADDR, REG_ACCEL_CONFIG, 0x00)) return false;
  if (!i2cWriteByte(MPU_ADDR, REG_SMPLRT_DIV, 9)) return false;
  i2cWriteByte(MPU_ADDR, REG_FIFO_EN, 0x00); 
  i2cWriteByte(MPU_ADDR, REG_INT_ENABLE, 0x00); 
  i2cWriteByte(MPU_ADDR, REG_INT_PIN_CFG, 0x00);
  delay(5); return true;
}

bool setLowPowerAccelWOM(uint8_t thrLSB = 25, uint8_t durMS = 40, uint8_t lpWake = 2) {
  uint8_t pwr2 = ((lpWake & 0x03) << 6) | 0x07;
  if (!i2cWriteByte(MPU_ADDR, REG_PWR_MGMT_2, pwr2)) return false;
  if (!i2cWriteByte(MPU_ADDR, REG_MOT_THR, thrLSB)) return false;
  if (!i2cWriteByte(MPU_ADDR, REG_MOT_DUR, durMS)) return false;
  i2cWriteByte(MPU_ADDR, REG_MOT_DETECT_CTRL, 0x15);
  i2cWriteByte(MPU_ADDR, REG_INT_PIN_CFG, 0x30);
  if (!i2cWriteByte(MPU_ADDR, REG_INT_ENABLE, 0x40)) return false;
  uint8_t pwr1 = 0x01 | 0x20;
  if (!i2cWriteByte(MPU_ADDR, REG_PWR_MGMT_1, pwr1)) return false;
  delay(5); return true;
}

bool readIntStatus(uint8_t &status) { 
  return i2cReadBytes(MPU_ADDR, REG_INT_STATUS, &status, 1); 
}

// ==================== VAR WINDOW ====================
void pushVar(float aMag) {
  if (aCount < VAR_WIN) { 
    aBuf[aCount++] = aMag; 
    aSum += aMag; 
    aSum2 += aMag * aMag; 
  } else { 
    float old = aBuf[aIdx]; 
    aBuf[aIdx] = aMag; 
    aIdx = (aIdx + 1) % VAR_WIN; 
    aSum += aMag - old; 
    aSum2 += aMag * aMag - old * old; 
  }
}

float getVar() { 
  int n = aCount < VAR_WIN ? aCount : VAR_WIN; 
  if (n <= 1) return 1e9; 
  float mean = aSum / n; 
  return (aSum2 / n) - mean * mean; 
}

// ==================== BUZZER (SAFE) ====================
void ensureBuzzerReady() {
  if (!buzzerReady) {
    ledcAttach(BUZZER_PIN, BUZZER_FREQ, BUZZER_RES);
    ledcWrite(BUZZER_PIN, 0);
    buzzerReady = true;
  }
}

void buzzerBeep(int times, int duration_ms, int pause_ms, int volume_percent) {
  int duty = map(volume_percent, 0, 100, 12, 120);
  for (int i = 0; i < times; i++) {
    ledcWrite(BUZZER_PIN, duty);
    delay(duration_ms);
    ledcWrite(BUZZER_PIN, 0);
    if (i < times - 1) delay(pause_ms);
  }
}

void buzzerBeepSafe(int times, int duration_ms, int pause_ms, int volume_percent) {
  if (buzzerLockout) return;
  ensureBuzzerReady();
  buzzerBeep(times, duration_ms, pause_ms, volume_percent);
}

// ==================== FALL FSM ====================
inline float rad2deg(float rad) { return rad * 57.2957795f; }

bool readMPU(int16_t &ax, int16_t &ay, int16_t &az, int16_t &gx, int16_t &gy, int16_t &gz) {
  uint8_t buf[14]; 
  if (!i2cReadBytes(MPU_ADDR, REG_ACCEL_XOUT_H, buf, 14)) return false;
  ax = (int16_t)((buf[0] << 8) | buf[1]); 
  ay = (int16_t)((buf[2] << 8) | buf[3]); 
  az = (int16_t)((buf[4] << 8) | buf[5]);
  gx = (int16_t)((buf[8] << 8) | buf[9]); 
  gy = (int16_t)((buf[10] << 8) | buf[11]); 
  gz = (int16_t)((buf[12] << 8) | buf[13]);
  return true;
}

void publishEvent(const char* type, int severity);  // fwd decl

void fallFSMStep(float aMag, float gMag, unsigned long now_ms) {
  float azNorm = (aMag > 0.1f) ? fabsf(g_az / aMag) : 1.0f;
  unsigned long fsm_dt = (lastFsmStepMs == 0) ? 0 : (now_ms - lastFsmStepMs);
  lastFsmStepMs = now_ms;
  
  switch (fstate) {
    case F_IDLE:
      angleOK = false; 
      cumulativeInactMs = 0;
      if (aMag < THR_FREEFALL_G) {
        if (!hadFreeFall) { tFreeFall = now_ms; hadFreeFall = true; }
      }
      if (aMag > THR_IMPACT_MIN_G && aMag < THR_IMPACT_MAX_G && gMag > THR_IMPACT_GYRO_G) {
        tImpact = now_ms; fstate = F_POSSIBLE;
        
        impactForce = aMag; 
        if (hadFreeFall && (now_ms - tFreeFall < WIN_FREEFALL_MS)) { 
           impactForce += 1.0f; 
        }
        hadFreeFall = false;
      }
      if (hadFreeFall && (now_ms - tFreeFall > 500)) hadFreeFall = false;
      break;
      
    case F_POSSIBLE: {
      bool isLying = (azNorm < THR_LYING_AZ);
      if (isLying) {
        angleOK = true; fstate = F_CONFIRM; cumulativeInactMs = 0;
      } else if (now_ms - tImpact > WIN_ANGLE_MS) {
        fstate = F_IDLE;
      }
    } break;
    
    case F_CONFIRM: {
      float aVar = getVar();
      if (gMag < THR_GYRO_IDLE && aVar < THR_AMAG_VAR) {
        cumulativeInactMs += fsm_dt;
      } else {
        cumulativeInactMs = (cumulativeInactMs > fsm_dt) ? (cumulativeInactMs - fsm_dt) : 0;
      }
      if (cumulativeInactMs >= REQ_INACT_MS) {
        fstate = F_FALL_LATCH; 
        tLatch = now_ms; 
        cumulativeInactMs = 0;
        
        int calculatedSeverity = 1;
        if (impactForce > 4.5f) {
           calculatedSeverity = 3; 
           buzzerBeepSafe(5, 100, 50, 100); 
        } else if (impactForce > 3.0f) {
           calculatedSeverity = 2; 
           buzzerBeepSafe(3, 300, 100, 50); 
        } else {
           calculatedSeverity = 1; 
           buzzerBeepSafe(2, 500, 200, 25); 
        }
        
        publishEvent("fall", calculatedSeverity); 
      }
      if (now_ms - tImpact > 8000 && fstate == F_CONFIRM) fstate = F_IDLE;
    } break;
    
    case F_FALL_LATCH:
      if (now_ms - tLatch > LATCH_MS) fstate = F_IDLE;
      break;
  }
}

// ==================== MAX30102 ====================
uint8_t mA_to_reg(float mA) {
  int v = (int)roundf(mA / 0.2f);
  if (v < 0) v = 0; if (v > 255) v = 255;
  return (uint8_t)v;
}

bool max30102_begin() {
  if (!particleSensor.begin(Wire)) return false;
  particleSensor.shutDown();
  particleSensor.setup();
  particleSensor.setLEDMode(2);
  particleSensor.setADCRange(16384);
  particleSensor.setSampleRate(100);
  particleSensor.setPulseWidth(411);
  particleSensor.setFIFOAverage(4);
  particleSensor.setPulseAmplitudeRed(mA_to_reg(curLED_RED_mA));
  particleSensor.setPulseAmplitudeIR(mA_to_reg(curLED_IR_mA));
  particleSensor.clearFIFO();
  return true;
}

void max30102_stop() { if (maxInited) particleSensor.shutDown(); }

void max30102_start() {
  if (!maxInited) {
    maxInited = max30102_begin();
    if (!maxInited) { Serial.println(F("MAX30102 init FAILED")); return; }
  }
  particleSensor.clearFIFO();
  particleSensor.wakeUp();
  delay(10);
  particleSensor.clearFIFO();
  delay(10);
}

void ppgResetSession() { 
  ppgCount = 0; validCount = 0; ppgPhaseStartMs = millis(); 
}

bool ppgSignalGood(uint32_t irDC, float gMag, float aVar) {
  return (irDC > IR_DC_MIN) && (gMag < MOTION_G_THR) && (aVar < AMAG_VAR_THR);
}

void autoGainAdjust(uint32_t irDC, uint32_t redDC) {
  static uint8_t rateLimiter = 0;
  if (++rateLimiter % 3 != 0) return;

  if (irDC < DC_TARGET_MIN && curLED_IR_mA < LED_IR_MAX) {
    curLED_IR_mA = min(curLED_IR_mA + 2, LED_IR_MAX);
    particleSensor.setPulseAmplitudeIR(mA_to_reg(curLED_IR_mA));
  } else if (irDC > DC_TARGET_MAX && curLED_IR_mA > LED_IR_MIN) {
    curLED_IR_mA = max(curLED_IR_mA - 2, LED_IR_MIN);
    particleSensor.setPulseAmplitudeIR(mA_to_reg(curLED_IR_mA));
  }

  if (redDC < DC_TARGET_MIN && curLED_RED_mA < LED_RED_MAX) {
    curLED_RED_mA = min(curLED_RED_mA + 2, LED_RED_MAX);
    particleSensor.setPulseAmplitudeRed(mA_to_reg(curLED_RED_mA));
  } else if (redDC > DC_TARGET_MAX && curLED_RED_mA > LED_RED_MIN) {
    curLED_RED_mA = max(curLED_RED_mA - 2, LED_RED_MIN);
    particleSensor.setPulseAmplitudeRed(mA_to_reg(curLED_RED_mA));
  }
}

bool computeHRSpO2_windowed(int count, float &hr, float &spo2) {
  if (count < SPO2_WIN) return false;
  int start = count - SPO2_WIN;
  for (int i = 0; i < SPO2_WIN; i++) {
    irWin[i]  = irBuf[start + i];
    redWin[i] = redBuf[start + i];
  }

  int32_t spo2Val = 0, hrVal = 0;
  int8_t  spo2Valid = 0, hrValid = 0;

  maxim_heart_rate_and_oxygen_saturation(
    (uint32_t*)irWin, (int32_t)SPO2_WIN,
    (uint32_t*)redWin,
    &spo2Val, &spo2Valid,
    &hrVal,   &hrValid
  );

  if (spo2Valid && hrValid && hrVal > 40 && hrVal < 180 && spo2Val >= 85 && spo2Val <= 100) {
    hr = (float)hrVal;
    spo2 = (float)spo2Val;
    return true;
  }
  return false;
}

// ==================== LOG HEADER ====================
void printHeaderOnce() {
  static bool printed = false; 
  if (!printed) { 
    Serial.println(F("time_ms,mode,aMag,gMag,roll,pitch,azNorm,state,ppg")); 
    printed = true; 
  }
}

// ==================== WIFI/MQTT HELPERS ====================
void wifiConnect() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("[WiFi] Connecting"));
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300); Serial.print('.');
    if (millis() - t0 > 15000) break;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WiFi] OK, IP=")); Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("[WiFi] FAIL"));
  }
}

void onMqttMsg(char* topic, byte* payload, unsigned int len) {
  String s; s.reserve(len+1);
  for (unsigned int i=0;i<len;i++) s += (char)payload[i];
  s.toLowerCase();
  Serial.print(F("[MQTT] cmd <- ")); Serial.println(s);

  bool doBtn1 = (s.indexOf("\"btn\":1")>=0) || (s.indexOf("\"btn1\":true")>=0);
  bool doBtn2 = (s.indexOf("\"btn\":2")>=0) || (s.indexOf("\"btn2\":true")>=0);

  if (doBtn1) {
    if (buzzerLockout) {
      Serial.println(F("[CMD] BTN1 ignored (boot mute)"));
    } else {
      Serial.println(F("[CMD] BTN1 -> beep 3x"));
      buzzerBeepSafe(3, 300, 100, 10);
    }
  }
  if (doBtn2) {
    if (buzzerLockout) {
      Serial.println(F("[CMD] BTN2 ignored (boot mute)"));
    } else {
      Serial.println(F("[CMD] BTN2 -> beep 5x"));
      buzzerBeepSafe(5, 300, 100, 10);
    }
  }
}

void mqttEnsureConnected() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttAttempt < MQTT_RETRY_MS) return;
  lastMqttAttempt = millis();

  Serial.print(F("[MQTT] Connecting to Cloud... "));
  String clientId = String("wear-") + DEV_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  
  // <<< SỬA: Thêm User và Pass vào hàm connect >>>
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) { 
    Serial.println(F("OK"));
    mqtt.subscribe(topicCmd.c_str());
    Serial.print(F("[MQTT] Subscribed: ")); Serial.println(topicCmd);
  } else {
    Serial.print(F("FAIL rc=")); Serial.println(mqtt.state());
  }
}

void publishData(float hr, float spo2, float batt_v /* = NAN nếu chưa đo */) {
  char buf[160];
  unsigned long ts = millis()/1000;
  if (isnan(batt_v))
    snprintf(buf, sizeof(buf), "{\"ts\":%lu,\"hr\":%.1f,\"spo2\":%.1f}", ts, hr, spo2);
  else
    snprintf(buf, sizeof(buf), "{\"ts\":%lu,\"hr\":%.1f,\"spo2\":%.1f,\"batt_v\":%.2f}", ts, hr, spo2, batt_v);

  if (mqtt.connected()) {
    // <<< QUAN TRỌNG: Sửa true -> false để tắt Retain >>>
    mqtt.publish(topicData.c_str(), buf, false); 
    Serial.print(F("[MQTT] data -> ")); Serial.println(buf);
  }
}

void publishEvent(const char* type, int severity) {
  char buf[128];
  unsigned long ts = millis()/1000;
  snprintf(buf, sizeof(buf), "{\"ts\":%lu,\"type\":\"%s\",\"severity\":%d}", ts, type, severity);
  if (mqtt.connected()) {
    // <<< QUAN TRỌNG: Sửa true -> false để tắt Retain >>>
    mqtt.publish(topicEvent.c_str(), buf, false);
    Serial.print(F("[MQTT] event -> ")); Serial.println(buf);
  }
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200); 
  delay(50);
  Serial.println();
  Serial.println(F("=== ESP32-C3 + MPU6050 OPTIMIZED FALL DETECTION (v2, NO-BOOT-BEEP) ==="));

  bootMs = millis();
  buzzerLockout = true;
  buzzerReady   = false;

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  Wire.setTimeOut(50); 
  delay(30);

  // === MPU INIT ===
  MPU_ADDR = scanPickMPUAddr();
  if (!setLowPowerAccelWOM(25, 40, 2)) { 
    i2cBusRecovery(); 
    MPU_ADDR = scanPickMPUAddr(); 
    setLowPowerAccelWOM(25, 40, 2); 
  }

  // === MAX30102 INIT ===
  maxInited = max30102_begin();
  if (maxInited) { 
    max30102_stop(); 
    Serial.println(F("✓ MAX30102 ready")); 
  } else {
    Serial.println(F("✗ MAX30102 not found"));
  }

  // ===== WIFI + MQTT =====
  wifiConnect();
  
  // <<< QUAN TRỌNG: Cấu hình bảo mật >>>
  espClient.setInsecure(); // Bỏ qua kiểm tra chứng chỉ
  
  // <<< QUAN TRỌNG: Tăng Buffer Size cho MQTT >>>
  mqtt.setBufferSize(4096); 

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMsg);

  topicData  = String("wearable/") + DEV_ID + "/data";
  topicEvent = String("wearable/") + DEV_ID + "/event";
  topicCmd   = String("wearable/") + DEV_ID + "/cmd";

  mqttEnsureConnected();

  lastUs = micros();
  Serial.println(F("=== READY (Muted during boot) ==="));
}

// ==================== LOOP ====================
void loop() {
  unsigned long nowUs = micros(); 
  float dt = (nowUs - lastUs) / 1e6f;
  if (dt < 0.005f) { 
    if (WiFi.status() != WL_CONNECTED) {
      static unsigned long lastWi = 0;
      if (millis() - lastWi > 10000) { wifiConnect(); lastWi = millis(); }
    }
    mqttEnsureConnected();
    if (mqtt.connected()) mqtt.loop();

    yield(); 
    return; 
  }
  lastUs = nowUs;
  unsigned long nowMs = millis();

  if (buzzerLockout && (nowMs - bootMs >= BOOT_MUTE_MS)) {
    buzzerLockout = false;
    ensureBuzzerReady();
    Serial.println(F("[BUZZER] Unmuted after boot-safe window."));
  }

  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWi = 0;
    if (millis() - lastWi > 10000) { wifiConnect(); lastWi = millis(); }
  }
  mqttEnsureConnected();
  if (mqtt.connected()) mqtt.loop();

  // === SERIAL COMMANDS ===
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '1') {
      if (buzzerLockout) {
        Serial.println(F("\n🍚 [MEAL] (Muted during boot window)"));
      } else {
        Serial.println(F("\n🍚 [MEAL] 3x 10%"));
        buzzerBeepSafe(3, 300, 100, 10);
      }
    } else if (c == '2') {
      if (buzzerLockout) {
        Serial.println(F("\n😴 [BED] (Muted during boot window)"));
      } else {
        Serial.println(F("\n😴 [BED] 5x 10%"));
        buzzerBeepSafe(5, 300, 100, 10);
      }
    } else {
      Serial.print(F("\n[?] Unknown: '")); 
      Serial.print(c); 
      Serial.println(F("'"));
    }
  }

  // === PPG SCHEDULER === (không ảnh hưởng buzzer)
  if (ppgState == PPG_IDLE) {
    if (nowMs >= ppgNextDueMs) {
      max30102_start();
      curLED_RED_mA = 30; curLED_IR_mA = 60;
      particleSensor.setPulseAmplitudeRed(mA_to_reg(curLED_RED_mA));
      particleSensor.setPulseAmplitudeIR(mA_to_reg(curLED_IR_mA));
      particleSensor.clearFIFO();
      delay(10);
      ppgResetSession();
      ppgState = PPG_WARMUP;
      Serial.println(F("[PPG] Warmup..."));
    }
  } else if (ppgState == PPG_WARMUP) {
    particleSensor.check();
    if (nowMs - ppgPhaseStartMs >= PPG_WARMUP_MS) {
      ppgPhaseStartMs = millis();
      ppgState = PPG_MEAS;
      Serial.println(F("[PPG] Measuring..."));
    }
  } else if (ppgState == PPG_MEAS) {
    particleSensor.check();
    while (particleSensor.available()) {
      uint32_t ir = particleSensor.getIR();
      uint32_t red = particleSensor.getRed();
      if (ppgCount < PPG_MAX_SAMPLES) { 
        irBuf[ppgCount] = ir; 
        redBuf[ppgCount] = red; 
        ppgCount++; 
      }
      particleSensor.nextSample();
    }

    static unsigned long lastMPUms = 0; 
    static float last_gMag = 0, last_aVar = 999;
    if (nowMs - lastMPUms >= 20) {
      if (mode == ACTIVE) {
        int16_t axR, ayR, azR, gxR, gyR, gzR;
        if (readMPU(axR, ayR, azR, gxR, gyR, gzR)) {
          float ax = axR / ACC_SCALE, ay = ayR / ACC_SCALE, az = azR / ACC_SCALE;
          float gx = gxR / GYR_SCALE, gy = gyR / GYR_SCALE, gz = gzR / GYR_SCALE;
          float aMag = sqrtf(ax * ax + ay * ay + az * az); 
          last_gMag = sqrtf(gx * gx + gy * gy + gz * gz);
          pushVar(aMag); 
          last_aVar = getVar();
        }
      } else { 
        last_gMag = 0; 
        last_aVar = 0; 
      }
      lastMPUms = nowMs;

      uint32_t irDC = (ppgCount > 0) ? irBuf[ppgCount - 1] : 0;
      uint32_t redDC = (ppgCount > 0) ? redBuf[ppgCount - 1] : 0;
      if (nowMs - ppgPhaseStartMs < 5000UL) autoGainAdjust(irDC, redDC);

      if (ppgCount >= SPO2_WIN) {
        uint32_t irDCnow = irBuf[ppgCount - 1];
        if (ppgSignalGood(irDCnow, last_gMag, last_aVar)) {
          float hr, sp;
          if (computeHRSpO2_windowed(ppgCount, hr, sp)) {
            if (validCount < 10) { 
              hrVec[validCount] = hr; 
              spo2Vec[validCount] = sp; 
              validCount++; 
            }
          }
        }
      }
    }

    bool timeOkMin = (nowMs - ppgPhaseStartMs >= PPG_MEAS_MIN_MS);
    bool timeOver  = (nowMs - ppgPhaseStartMs >= PPG_MEAS_MAX_MS);
    bool locked = false;

    if (validCount >= 3) {
      float hrMin = 1000, hrMax = -1000, spMin = 1000, spMax = -1000;
      int take = min(validCount, 5);
      for (int i = validCount - take; i < validCount; i++) {
        hrMin = min(hrMin, hrVec[i]); hrMax = max(hrMax, hrVec[i]);
        spMin = min(spMin, spo2Vec[i]); spMax = max(spMax, spo2Vec[i]);
      }
      if ((hrMax - hrMin) <= 12.0 && (spMax - spMin) <= 5.0) locked = true;
    }

    if ((timeOkMin && locked) || timeOver) {
      if (validCount >= 1) {
        int take = min(validCount, 5);
        float tmpHR[5], tmpSp[5];
        for (int i = 0; i < take; i++) { 
          tmpHR[i] = hrVec[validCount - take + i]; 
          tmpSp[i] = spo2Vec[validCount - take + i]; 
        }
        for (int i = 0; i < take - 1; i++) 
          for (int j = i + 1; j < take; j++) 
            if (tmpHR[j] < tmpHR[i]) { float t = tmpHR[i]; tmpHR[i] = tmpHR[j]; tmpHR[j] = t; }
        for (int i = 0; i < take - 1; i++) 
          for (int j = i + 1; j < take; j++) 
            if (tmpSp[j] < tmpSp[i]) { float t = tmpSp[i]; tmpSp[i] = tmpSp[j]; tmpSp[j] = t; }
        float hrMed = tmpHR[take / 2], spMed = tmpSp[take / 2];

        Serial.print(F("[PPG] HR=")); Serial.print(hrMed, 1);
        Serial.print(F(" bpm, SpO2=")); Serial.print(spMed, 1);
        Serial.println(F(" %"));

        // Publish HR/SpO2
        publishData(hrMed, spMed, NAN /* TODO: đo pin nếu có */);
      }
      max30102_stop();
      ppgState = PPG_DONE;
      ppgNextDueMs = nowMs + PPG_PERIOD_MS;
    }
  } else if (ppgState == PPG_DONE) {
    if (nowMs >= ppgNextDueMs) ppgState = PPG_IDLE;
  }

  // === MPU PIPELINE ===
  if (mode == IDLE_LP) {
    static unsigned long lastPoll = 0;
    if (nowMs - lastPoll >= 25) {
      uint8_t status = 0;
      if (!readIntStatus(status)) {
        if (++readFailCount >= 5) { 
          i2cBusRecovery(); 
          setLowPowerAccelWOM(25, 40, 2); 
          readFailCount = 0; 
        }
      } else {
        readFailCount = 0;
        if (status & 0x40) { 
          setActiveMode100Hz(); 
          mode = ACTIVE; 
          quietStartMs = 0; 
          aIdx = 0; aCount = 0; aSum = 0; aSum2 = 0; 
          delay(5); 
        }
      }
      lastPoll = nowMs;
    }
    if (nowMs - lastPrintMs >= 250) {
      printHeaderOnce();
      Serial.print(nowMs); Serial.print(",IDLE,0,0,0,0,0,IDLE,");
      Serial.println((ppgState == PPG_MEAS) ? "PPG" : "-");
      lastPrintMs = nowMs;
    }
  } else { // ACTIVE
    static unsigned long lastReadUs = 0; 
    static unsigned long prevReadUs = 0;
    if ((nowUs - lastReadUs) >= 9000) {
      prevReadUs = lastReadUs; 
      lastReadUs = nowUs;
      float dtSample = (prevReadUs == 0) ? 0.01f : (lastReadUs - prevReadUs) / 1e6f;

      int16_t axR, ayR, azR, gxR, gyR, gzR;
      if (!readMPU(axR, ayR, azR, gxR, gyR, gzR)) {
        if (++readFailCount >= 5) { 
          i2cBusRecovery(); 
          setActiveMode100Hz(); 
          readFailCount = 0; 
        }
        yield(); return;
      }
      readFailCount = 0;

      float ax = axR / ACC_SCALE, ay = ayR / ACC_SCALE, az = azR / ACC_SCALE;
      float gx = gxR / GYR_SCALE, gy = gyR / GYR_SCALE, gz = gzR / GYR_SCALE;
      g_ax = ax; g_ay = ay; g_az = az;

      float aMag = sqrtf(ax * ax + ay * ay + az * az);
      float gMag = sqrtf(gx * gx + gy * gy + gz * gz);

      float rollAcc  = rad2deg(atan2f(ay, az));
      float pitchAcc = rad2deg(atan2f(-ax, sqrtf(ay * ay + az * az)));
      roll_deg  = COMP_ALPHA * (roll_deg + gx * dtSample) + (1.0f - COMP_ALPHA) * rollAcc;
      pitch_deg = COMP_ALPHA * (pitch_deg + gy * dtSample) + (1.0f - COMP_ALPHA) * pitchAcc;

      pushVar(aMag);
      fallFSMStep(aMag, gMag, nowMs);

      float aVar = getVar();
      bool isQuiet = (gMag < QUIET_GYRO_THR) && (aVar < QUIET_AMAG_VAR) && 
                       (fstate == F_IDLE || fstate == F_FALL_LATCH);
      if (isQuiet) { 
        if (quietStartMs == 0) quietStartMs = nowMs; 
      } else quietStartMs = 0;
      if (quietStartMs && (nowMs - quietStartMs >= ACTIVE_QUIET_BACK_MS)) {
        setLowPowerAccelWOM(25, 40, 2); 
        mode = IDLE_LP; 
        quietStartMs = 0;
        Serial.println(F("<< Back to Low Power >>"));
      }

      if (nowMs - lastPrintMs >= 20) {
        printHeaderOnce();
        float azNorm = (aMag > 0.1f) ? fabsf(az / aMag) : 1.0f;
        Serial.print(nowMs); Serial.print(",ACTIVE,");
        Serial.print(aMag, 3); Serial.print(",");
        Serial.print(gMag, 1); Serial.print(",");
        Serial.print(roll_deg, 1); Serial.print(",");
        Serial.print(pitch_deg, 1); Serial.print(",");
        Serial.print(azNorm, 3); Serial.print(",");
        switch (fstate) { 
          case F_IDLE: Serial.print("IDLE"); break; 
          case F_POSSIBLE: Serial.print("POSSIBLE"); break; 
          case F_CONFIRM: Serial.print("CONFIRM"); break; 
          case F_FALL_LATCH: Serial.print("FALL"); break; 
        }
        Serial.print(",");
        Serial.println((ppgState == PPG_MEAS) ? "PPG" : "-");
        lastPrintMs = nowMs;
      }
    }
  }

  yield();
}