/*
 * ╔══════════════════════════════════════════════════════════════════╗
 *  SAAK-GUARD  v4.1  —  FORENSIC EDITION
 *  Voice Trigger + Touch Trigger → 30s Encrypted Stream to Backend
 *
 *  Hardware : XIAO ESP32-S3 Sense + A9G GSM/GPS Module
 *  Board    : XIAO_ESP32S3
 *  PSRAM    : Tools → PSRAM → OPI PSRAM        ← MUST enable
 *  Partition: Tools → Partition → Huge APP (3MB No OTA)
 *
 *  Required Libraries (install via Arduino Library Manager):
 *    - esp32-camera  (comes with ESP32 board package)
 *    - ArduinoJson   (Benoit Blanchon)
 *    - WiFiClientSecure (built-in)
 *    - HTTPClient       (built-in)
 *    - mbedtls          (built-in, for AES-256 + SHA-256)
 *
 * ╚══════════════════════════════════════════════════════════════════╝
 *
 *  ┌──────────────────── WIRING DIAGRAM ────────────────────────────┐
 *  │                                                                │
 *  │  TTP223 Touch Sensor:                                          │
 *  │    SIG  →  D0 / GPIO 1  (via 10kΩ pull-down to GND)           │
 *  │    VCC  →  3.3V                                                │
 *  │    GND  →  GND                                                 │
 *  │                                                                │
 *  │  A9G GSM/GPS Module:                                           │
 *  │    VCC      →  3.3V rail  (100µF cap between VCC and GND)      │
 *  │    GND      →  GND (star ground)                               │
 *  │    PWRKEY   →  D1 / GPIO 2  (via 1kΩ) — pull HIGH 500ms       │
 *  │    RST      →  D2 / GPIO 3  (via 1kΩ) — active LOW reset      │
 *  │    RX pin   →  D9 / GPIO 8  (UART1 TX out from XIAO)           │
 *  │    TX pin   →  D10/ GPIO 9  (UART1 RX into XIAO)               │
 *  │             A9G TX uses: 10kΩ→GND divider, then 1kΩ to GPIO9  │
 *  │    NET_STATUS→ 10kΩ to GND (optional, monitor only)            │
 *  │                                                                │
 *  │  !! GPIO 43 (D6) and GPIO 44 (D7) = USB Serial — DO NOT USE!! │
 *  │                                                                │
 *  │  Camera + Mic: on-board XIAO Sense — no wiring needed          │
 *  └────────────────────────────────────────────────────────────────┘
 *
 *  HOW TO TRIGGER SOS:
 *    • Double-tap the TTP223 sensor, OR
 *    • Hold it for 2 seconds, OR
 *    • 3 loud claps/shouts within 3 seconds
 *
 *  WHAT HAPPENS:
 *    1. A9G reads GPS coordinates
 *    2. A9G sends SMS (Google Maps link) to all emergency contacts
 *    3. A9G calls the primary contact for 30 seconds
 *    4. Camera captures JPEG frames at ~10fps for 30s (AES-256 encrypted)
 *    5. Mic records 30s of audio, uploads as WAV to backend
 *    6. Backend applies forensic watermark, assembles MP4, signs evidence
 *
 *  CONFIGURATION:
 *    Edit the USER CONFIGURATION block below before flashing.
 */

// ════════════════════════════════════════════════════════════
//  INCLUDES
// ════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "esp_camera.h"
#include "driver/i2s.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

// ════════════════════════════════════════════════════════════
//  ██ USER CONFIGURATION — EDIT BEFORE FLASHING ██
// ════════════════════════════════════════════════════════════

// WiFi
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Backend (your Render URL, no trailing slash)
const char* SERVER_HOST   = "https://your-app.onrender.com";
const char* DEVICE_ID     = "SAAK-001";

// AES-256 key — must be exactly 64 hex characters (= 32 bytes)
// Generate: python -c "import secrets; print(secrets.token_hex(32))"
const char* AES_KEY_HEX = "097f46d48570383e03f7509bad7f0c83408b86e4718def132a7e7d3f6e0c35e5";

// Emergency contacts — primary gets call + SMS, rest get SMS only
const char* EMERGENCY_CONTACTS[] = {
  "+91XXXXXXXXXX",   // Primary
  "+91XXXXXXXXXX",   // Secondary
};
const int NUM_CONTACTS = 2;

// ════════════════════════════════════════════════════════════
//  PIN DEFINITIONS  (matches your wiring diagram exactly)
// ════════════════════════════════════════════════════════════

// TTP223 touch sensor signal
#define PIN_TTP223        1    // D0 / GPIO 1 — via 10kΩ pull-down

// A9G control pins
#define A9G_PWR_PIN       2    // D1 / GPIO 2 — PWRKEY via 1kΩ
#define A9G_RST_PIN       3    // D2 / GPIO 3 — RST    via 1kΩ

// A9G UART — HardwareSerial(1) = UART1
// XIAO D9/GPIO8  = UART1 TX → A9G RX pin
// XIAO D10/GPIO9 = UART1 RX ← A9G TX pin (through voltage divider)
#define A9G_UART_TX_PIN   8    // D9  / GPIO 8
#define A9G_UART_RX_PIN   9    // D10 / GPIO 9

// Camera — OV2640 on-board (do not change)
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK     10
#define CAM_PIN_SIOD     40
#define CAM_PIN_SIOC     39
#define CAM_PIN_D7       48
#define CAM_PIN_D6       11
#define CAM_PIN_D5       12
#define CAM_PIN_D4       14
#define CAM_PIN_D3       16
#define CAM_PIN_D2       18
#define CAM_PIN_D1       17
#define CAM_PIN_D0       15
#define CAM_PIN_VSYNC    38
#define CAM_PIN_HREF     47
#define CAM_PIN_PCLK     13

// Mic — PDM on-board (do not change)
#define MIC_CLK_PIN      42
#define MIC_DATA_PIN     41

// ════════════════════════════════════════════════════════════
//  SETTINGS
// ════════════════════════════════════════════════════════════

#define DOUBLE_TAP_WINDOW_MS   600
#define HOLD_MS               2000
#define RECORD_SECONDS          30

#define SAMPLE_RATE          16000
#define BITS_PER_SAMPLE         16
#define I2S_PORT          I2S_NUM_0
#define I2S_DMA_BUF_COUNT        8
#define I2S_DMA_BUF_LEN       1024

#define CAM_FRAME_SIZE  FRAMESIZE_QVGA    // 320×240
#define CAM_QUALITY              12       // 0-63

// Voice pattern trigger thresholds
#define VOICE_THRESHOLD       2000        // amplitude (0-32767)
#define VOICE_CLAPS_NEEDED       3        // loud bursts needed
#define VOICE_WINDOW_MS       3000        // detection window (ms)

// ════════════════════════════════════════════════════════════
//  GLOBALS
// ════════════════════════════════════════════════════════════
volatile bool     sosFlag   = false;
volatile bool     recording = false;
SemaphoreHandle_t sosMutex  = NULL;

bool camReady  = false;
bool micReady  = false;
bool wifiReady = false;
bool a9gReady  = false;

String g_sessionId = "";
float  g_gpsLat    = 0.0f;
float  g_gpsLon    = 0.0f;

HardwareSerial A9G(1);   // UART1 — NOT UART0 (USB Serial)
uint8_t        aesKey[32];

// ════════════════════════════════════════════════════════════
//  UTILITY: hex string → byte array
// ════════════════════════════════════════════════════════════
void hexToBytes(const char* hex, uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char b[3] = { hex[i*2], hex[i*2+1], 0 };
    out[i] = (uint8_t)strtol(b, NULL, 16);
  }
}

// ════════════════════════════════════════════════════════════
//  UTILITY: SHA-256 → hex string
// ════════════════════════════════════════════════════════════
String sha256Hex(const uint8_t* data, size_t len) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, data, len);
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  String out = "";
  for (int i = 0; i < 32; i++) {
    if (hash[i] < 0x10) out += "0";
    out += String(hash[i], HEX);
  }
  return out;
}

// ════════════════════════════════════════════════════════════
//  UTILITY: AES-256-CBC encrypt (PKCS#7 padding)
// ════════════════════════════════════════════════════════════
size_t aes256Encrypt(const uint8_t* in, size_t inLen,
                     uint8_t* out, const uint8_t* iv) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, aesKey, 256);

  size_t   padLen = 16 - (inLen % 16);
  size_t   padded = inLen + padLen;
  uint8_t* tmp    = (uint8_t*)malloc(padded);
  if (!tmp) { mbedtls_aes_free(&aes); return 0; }
  memcpy(tmp, in, inLen);
  memset(tmp + inLen, (uint8_t)padLen, padLen);

  uint8_t ivCopy[16];
  memcpy(ivCopy, iv, 16);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, ivCopy, tmp, out);
  free(tmp);
  mbedtls_aes_free(&aes);
  return padded;
}

// ════════════════════════════════════════════════════════════
//  A9G: send AT command, return response
// ════════════════════════════════════════════════════════════
String a9gSendAT(const String& cmd, uint32_t timeoutMs = 2000) {
  A9G.println(cmd);
  String   resp  = "";
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs) {
    while (A9G.available()) resp += (char)A9G.read();
    if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) break;
    delay(10);
  }
  Serial.printf("[A9G] %s → %s\n", cmd.c_str(), resp.c_str());
  return resp;
}

// ════════════════════════════════════════════════════════════
//  A9G: Power-on sequence
//  PWRKEY: HIGH for 500ms to boot
//  RST:    stay HIGH during normal use (active-LOW reset)
// ════════════════════════════════════════════════════════════
bool a9gInit() {
  pinMode(A9G_PWR_PIN, OUTPUT);
  pinMode(A9G_RST_PIN, OUTPUT);

  digitalWrite(A9G_RST_PIN, HIGH);   // not in reset
  delay(100);

  // Power-on pulse
  digitalWrite(A9G_PWR_PIN, LOW);
  delay(100);
  digitalWrite(A9G_PWR_PIN, HIGH);
  delay(500);
  digitalWrite(A9G_PWR_PIN, LOW);

  Serial.println("[A9G] Booting (~5s)...");
  delay(5000);

  // UART1: TX=GPIO8(D9), RX=GPIO9(D10)
  A9G.begin(115200, SERIAL_8N1, A9G_UART_RX_PIN, A9G_UART_TX_PIN);
  delay(500);

  String r = a9gSendAT("AT", 3000);
  if (r.indexOf("OK") < 0) {
    Serial.println("[A9G] No response — verify: D9→A9G_RX, D10→A9G_TX");
    return false;
  }

  a9gSendAT("ATE0");        // disable echo
  a9gSendAT("AT+CMGF=1");  // SMS text mode
  a9gSendAT("AT+GPS=1");   // enable GPS

  Serial.println("[A9G] OK");
  return true;
}

// ════════════════════════════════════════════════════════════
//  A9G: Get GPS fix
// ════════════════════════════════════════════════════════════
bool a9gGetGPS() {
  String resp = a9gSendAT("AT+LOCATION=2", 8000);
  int idx = resp.indexOf("+LOCATION:");
  if (idx < 0) { Serial.println("[GPS] No fix"); return false; }
  String data = resp.substring(idx + 10);
  int comma   = data.indexOf(',');
  if (comma < 0) return false;
  g_gpsLat = data.substring(0, comma).toFloat();
  g_gpsLon = data.substring(comma + 1).toFloat();
  Serial.printf("[GPS] %.6f, %.6f\n", g_gpsLat, g_gpsLon);
  return true;
}

// ════════════════════════════════════════════════════════════
//  A9G: SMS to all contacts
// ════════════════════════════════════════════════════════════
void a9gSendSMS() {
  String msg = "SOS ALERT — SAAK-Guard\n";
  msg += "Device: " + String(DEVICE_ID) + "\n";
  msg += (g_gpsLat != 0.0f)
    ? "Location: https://maps.google.com/?q=" + String(g_gpsLat,6) + "," + String(g_gpsLon,6)
    : "GPS: Acquiring...";
  msg += "\nSession: " + g_sessionId;

  for (int i = 0; i < NUM_CONTACTS; i++) {
    A9G.println("AT+CMGS=\"" + String(EMERGENCY_CONTACTS[i]) + "\"");
    delay(500);
    A9G.print(msg);
    A9G.write(26);    // Ctrl+Z
    delay(3000);
    Serial.printf("[SMS] Sent to %s\n", EMERGENCY_CONTACTS[i]);
  }
}

// ════════════════════════════════════════════════════════════
//  A9G: Call primary contact for 30s then hang up
// ════════════════════════════════════════════════════════════
void a9gCall() {
  if (NUM_CONTACTS == 0) return;
  a9gSendAT("ATD" + String(EMERGENCY_CONTACTS[0]) + ";", 1000);
  Serial.printf("[CALL] Calling %s\n", EMERGENCY_CONTACTS[0]);
  vTaskDelay(pdMS_TO_TICKS(30000));
  a9gSendAT("ATH");
  Serial.println("[CALL] Hung up");
}

// ════════════════════════════════════════════════════════════
//  WiFi: Connect
// ════════════════════════════════════════════════════════════
bool wifiConnect() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
    Serial.print(".");
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("\n[WiFi] Failed");
  return false;
}

// ════════════════════════════════════════════════════════════
//  Backend: Register session → get session ID
// ════════════════════════════════════════════════════════════
bool backendRegisterSession(const char* trigger) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, String(SERVER_HOST) + "/api/sos/register");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-ID",  DEVICE_ID);

  DynamicJsonDocument doc(256);
  doc["device_id"] = DEVICE_ID;
  doc["gps_lat"]   = g_gpsLat;
  doc["gps_lon"]   = g_gpsLon;
  doc["trigger"]   = trigger;
  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code == 200) {
    DynamicJsonDocument rDoc(256);
    deserializeJson(rDoc, http.getString());
    g_sessionId = rDoc["session_id"].as<String>();
    Serial.printf("[API] Session: %s\n", g_sessionId.c_str());
    http.end();
    return true;
  }
  http.end();
  Serial.printf("[API] Register failed: %d\n", code);
  return false;
}

// ════════════════════════════════════════════════════════════
//  Backend: Stream one AES-256 encrypted JPEG frame
// ════════════════════════════════════════════════════════════
void backendStreamFrame(const uint8_t* jpeg, size_t jpegLen, uint32_t num) {
  if (WiFi.status() != WL_CONNECTED || g_sessionId.isEmpty()) return;

  uint8_t iv[16];
  esp_fill_random(iv, 16);

  size_t   maxEnc = jpegLen + 32;
  uint8_t* enc    = (uint8_t*)malloc(maxEnc);
  if (!enc) return;
  size_t encLen = aes256Encrypt(jpeg, jpegLen, enc, iv);

  String ivHex = "", frameHash = sha256Hex(jpeg, jpegLen);
  for (int i = 0; i < 16; i++) {
    if (iv[i] < 0x10) ivHex += "0";
    ivHex += String(iv[i], HEX);
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, String(SERVER_HOST) + "/api/sos/frame");
  http.addHeader("Content-Type",  "application/octet-stream");
  http.addHeader("X-Device-ID",   DEVICE_ID);
  http.addHeader("X-Session-ID",  g_sessionId);
  http.addHeader("X-Frame-Num",   String(num));
  http.addHeader("X-GPS-Lat",     String(g_gpsLat, 6));
  http.addHeader("X-GPS-Lon",     String(g_gpsLon, 6));
  http.addHeader("X-Frame-Hash",  frameHash);
  http.addHeader("X-IV",          ivHex);
  http.POST(enc, encLen);
  http.end();
  free(enc);
}

// ════════════════════════════════════════════════════════════
//  Backend: Upload full WAV audio buffer
// ════════════════════════════════════════════════════════════
void backendUploadAudio(const uint8_t* wav, size_t len) {
  if (WiFi.status() != WL_CONNECTED || g_sessionId.isEmpty()) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, String(SERVER_HOST) + "/api/sos/audio");
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Device-ID",  DEVICE_ID);
  http.addHeader("X-Session-ID", g_sessionId);
  int code = http.POST(const_cast<uint8_t*>(wav), len);
  http.end();
  Serial.printf("[AUDIO] Upload → HTTP %d\n", code);
}

// ════════════════════════════════════════════════════════════
//  Camera: Init
// ════════════════════════════════════════════════════════════
bool cameraInit() {
  camera_config_t cfg;
  cfg.ledc_channel  = LEDC_CHANNEL_0;  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0        = CAM_PIN_D0;      cfg.pin_d1       = CAM_PIN_D1;
  cfg.pin_d2        = CAM_PIN_D2;      cfg.pin_d3       = CAM_PIN_D3;
  cfg.pin_d4        = CAM_PIN_D4;      cfg.pin_d5       = CAM_PIN_D5;
  cfg.pin_d6        = CAM_PIN_D6;      cfg.pin_d7       = CAM_PIN_D7;
  cfg.pin_xclk      = CAM_PIN_XCLK;   cfg.pin_pclk     = CAM_PIN_PCLK;
  cfg.pin_vsync     = CAM_PIN_VSYNC;   cfg.pin_href     = CAM_PIN_HREF;
  cfg.pin_sscb_sda  = CAM_PIN_SIOD;   cfg.pin_sscb_scl = CAM_PIN_SIOC;
  cfg.pin_pwdn      = CAM_PIN_PWDN;   cfg.pin_reset     = CAM_PIN_RESET;
  cfg.xclk_freq_hz  = 20000000;
  cfg.pixel_format  = PIXFORMAT_JPEG;
  cfg.frame_size    = CAM_FRAME_SIZE;
  cfg.jpeg_quality  = CAM_QUALITY;
  cfg.fb_count      = 2;
  cfg.fb_location   = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;

  if (esp_camera_init(&cfg) != ESP_OK) {
    Serial.println("[CAM] Init failed — is PSRAM set to OPI PSRAM?");
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  s->set_brightness(s, 1); s->set_contrast(s, 1);
  s->set_whitebal(s, 1);   s->set_awb_gain(s, 1);
  s->set_aec2(s, 1);       s->set_gain_ctrl(s, 1);
  s->set_raw_gma(s, 1);    s->set_lenc(s, 1);
  Serial.println("[CAM] OK");
  return true;
}

// ════════════════════════════════════════════════════════════
//  Mic: Init (I2S PDM, on-board)
// ════════════════════════════════════════════════════════════
bool micInit() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_PCM_SHORT,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = I2S_DMA_BUF_COUNT,
    .dma_buf_len          = I2S_DMA_BUF_LEN,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = I2S_PIN_NO_CHANGE,
    .ws_io_num    = MIC_CLK_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_DATA_PIN
  };
  if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK ||
      i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
    Serial.println("[MIC] Init failed");
    i2s_driver_uninstall(I2S_PORT);
    return false;
  }
  i2s_zero_dma_buffer(I2S_PORT);
  Serial.println("[MIC] OK");
  return true;
}

// ════════════════════════════════════════════════════════════
//  WAV header writer
// ════════════════════════════════════════════════════════════
void writeWavHeader(uint8_t* buf, uint32_t dataBytes) {
  uint32_t fileSize = 36 + dataBytes;
  uint32_t byteRate = SAMPLE_RATE * (BITS_PER_SAMPLE / 8);
  uint16_t blockAln = BITS_PER_SAMPLE / 8;
  uint16_t bps      = BITS_PER_SAMPLE;
  uint16_t ch       = 1;
  uint16_t fmt      = 1;
  uint32_t fmtSz    = 16;
  uint32_t sr       = SAMPLE_RATE;
  memcpy(buf,      "RIFF", 4); memcpy(buf+4,  &fileSize, 4);
  memcpy(buf+8,    "WAVE", 4); memcpy(buf+12, "fmt ", 4);
  memcpy(buf+16,   &fmtSz, 4); memcpy(buf+20, &fmt, 2);
  memcpy(buf+22,   &ch, 2);    memcpy(buf+24, &sr, 4);
  memcpy(buf+28,   &byteRate,4); memcpy(buf+32, &blockAln, 2);
  memcpy(buf+34,   &bps, 2);   memcpy(buf+36, "data", 4);
  memcpy(buf+40,   &dataBytes, 4);
}

// ════════════════════════════════════════════════════════════
//  TASK: Camera — captures + streams frames for RECORD_SECONDS
// ════════════════════════════════════════════════════════════
void CameraRecordTask(void* pvParams) {
  uint32_t frameNum = 0;
  uint32_t startMs  = millis();
  uint32_t durMs    = (uint32_t)RECORD_SECONDS * 1000UL;
  uint32_t lastGPS  = 0;

  Serial.println("[CAM] Recording...");

  while ((millis() - startMs) < durMs) {
    if ((millis() - lastGPS) > 5000) {    // refresh GPS every 5s
      a9gGetGPS();
      lastGPS = millis();
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    backendStreamFrame(fb->buf, fb->len, ++frameNum);
    esp_camera_fb_return(fb);
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  Serial.printf("[CAM] Done — %lu frames\n", (unsigned long)frameNum);
  vTaskDelete(NULL);
}

// ════════════════════════════════════════════════════════════
//  TASK: Mic — records into PSRAM then uploads WAV
// ════════════════════════════════════════════════════════════
void MicRecordTask(void* pvParams) {
  // 30s × 16000Hz × 2 bytes = 960 000 bytes ≈ 937 KB in PSRAM
  const uint32_t dataBytes = RECORD_SECONDS * SAMPLE_RATE * (BITS_PER_SAMPLE / 8);
  const uint32_t bufLen    = 44 + dataBytes;

  uint8_t* buf = (uint8_t*)ps_malloc(bufLen);
  if (!buf) {
    Serial.println("[MIC] PSRAM alloc failed");
    vTaskDelete(NULL);
    return;
  }
  writeWavHeader(buf, 0);   // placeholder header

  uint32_t totalRead = 0;
  uint32_t startMs   = millis();
  uint32_t durMs     = (uint32_t)RECORD_SECONDS * 1000UL;

  Serial.println("[MIC] Recording...");

  while ((millis() - startMs) < durMs && totalRead < dataBytes) {
    size_t   n = 0;
    uint32_t remain = dataBytes - totalRead;
    uint32_t chunk  = (remain > 1024) ? 1024 : remain;
    i2s_read(I2S_PORT, buf + 44 + totalRead, chunk, &n, pdMS_TO_TICKS(100));
    totalRead += n;
  }

  writeWavHeader(buf, totalRead);   // fix header with real size
  Serial.printf("[MIC] Done — %.1f KB\n", totalRead / 1024.0f);

  backendUploadAudio(buf, 44 + totalRead);
  free(buf);
  vTaskDelete(NULL);
}

// ════════════════════════════════════════════════════════════
//  TASK: A9G — GPS, SMS, Call (parallel to A/V recording)
// ════════════════════════════════════════════════════════════
void A9GTask(void* pvParams) {
  if (a9gReady) {
    a9gGetGPS();
    a9gSendSMS();
    a9gCall();
  }
  vTaskDelete(NULL);
}

// ════════════════════════════════════════════════════════════
//  SOS HANDLER
// ════════════════════════════════════════════════════════════
void handleSOS(const char* trigger) {
  Serial.printf("\n===== SOS TRIGGERED [%s] =====\n", trigger);
  recording = true;

  backendRegisterSession(trigger);

  xTaskCreatePinnedToCore(A9GTask,          "A9G", 8192, NULL, 2, NULL, 0);
  if (camReady)
    xTaskCreatePinnedToCore(CameraRecordTask, "Cam", 8192, NULL, 2, NULL, 0);
  if (micReady)
    xTaskCreatePinnedToCore(MicRecordTask,   "Mic", 8192, NULL, 2, NULL, 0);

  // Wait for recording + upload to finish
  vTaskDelay(pdMS_TO_TICKS((RECORD_SECONDS + 10) * 1000));

  // Notify backend to finalize evidence
  if (WiFi.status() == WL_CONNECTED && !g_sessionId.isEmpty()) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, String(SERVER_HOST) + "/api/sos/end");
    http.addHeader("X-Device-ID",  DEVICE_ID);
    http.addHeader("X-Session-ID", g_sessionId);
    http.POST("{}");
    http.end();
  }

  recording   = false;
  g_sessionId = "";
  Serial.println("===== SOS Done =====\n");
}

// ════════════════════════════════════════════════════════════
//  TASK: Voice wake — 3 loud bursts in 3s = trigger
// ════════════════════════════════════════════════════════════
void VoiceWakeTask(void* pvParams) {
  Serial.println("[Voice] Ready — 3 claps/shouts in 3s = SOS");

  const size_t CHUNK = 512;
  int16_t  buf[CHUNK];
  int      count   = 0;
  uint32_t lastHit = 0;
  bool     inBurst = false;

  for (;;) {
    if (recording) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

    size_t n = 0;
    i2s_read(I2S_PORT, buf, CHUNK * sizeof(int16_t), &n, pdMS_TO_TICKS(50));

    if (n > 0) {
      int16_t peak = 0;
      for (size_t i = 0; i < n/2; i++) {
        int16_t v = abs(buf[i]);
        if (v > peak) peak = v;
      }

      if (peak > VOICE_THRESHOLD && !inBurst) {
        inBurst = true;
        uint32_t now = millis();
        if ((now - lastHit) > 150) {
          lastHit = now;
          count++;
          Serial.printf("[Voice] Burst %d/%d\n", count, VOICE_CLAPS_NEEDED);
        }
      }
      if (peak < VOICE_THRESHOLD / 2) inBurst = false;

      if (count > 0 && (millis() - lastHit) > VOICE_WINDOW_MS) {
        count = 0;
      }

      if (count >= VOICE_CLAPS_NEEDED) {
        count = 0;
        Serial.println("[Voice] Pattern → SOS");
        if (xSemaphoreTake(sosMutex, 0) == pdTRUE) {
          sosFlag = true;
          xSemaphoreGive(sosMutex);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ════════════════════════════════════════════════════════════
//  TASK: Touch — double-tap or 2s hold
// ════════════════════════════════════════════════════════════
void TouchTask(void* pvParams) {
  pinMode(PIN_TTP223, INPUT);
  bool     prev       = false;
  uint32_t pressStart = 0;
  uint32_t lastTapAt  = 0;
  uint8_t  tapCount   = 0;
  bool     holdFired  = false;

  Serial.println("[Touch] Ready");

  for (;;) {
    bool curr = (bool)digitalRead(PIN_TTP223);

    if (curr && !prev) {
      pressStart = millis();
      holdFired  = false;
    }

    if (curr && !holdFired && (millis() - pressStart) >= HOLD_MS) {
      holdFired = tapCount = 0;
      Serial.println("[Touch] Hold → SOS");
      if (!recording && xSemaphoreTake(sosMutex, 0) == pdTRUE) {
        sosFlag = true;
        xSemaphoreGive(sosMutex);
      }
    }

    if (!curr && prev && !holdFired && (millis() - pressStart) < HOLD_MS) {
      tapCount++;
      lastTapAt = millis();
    }

    if (tapCount >= 2) {
      tapCount = 0;
      Serial.println("[Touch] Double-tap → SOS");
      if (!recording && xSemaphoreTake(sosMutex, 0) == pdTRUE) {
        sosFlag = true;
        xSemaphoreGive(sosMutex);
      }
    }

    if (tapCount > 0 && !curr && (millis() - lastTapAt) > DOUBLE_TAP_WINDOW_MS) {
      tapCount = 0;
    }

    prev = curr;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ════════════════════════════════════════════════════════════
//  TASK: Main — watches sosFlag, fires handleSOS
// ════════════════════════════════════════════════════════════
void MainTask(void* pvParams) {
  for (;;) {
    bool fire = false;
    if (xSemaphoreTake(sosMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      fire    = sosFlag;
      sosFlag = false;
      xSemaphoreGive(sosMutex);
    }
    if (fire) handleSOS("trigger");
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("SAAK-Guard v4.1 starting...");

  hexToBytes(AES_KEY_HEX, aesKey, 32);
  sosMutex = xSemaphoreCreateMutex();

  camReady  = cameraInit();
  micReady  = micInit();
  wifiReady = wifiConnect();
  a9gReady  = a9gInit();

  Serial.printf("\n[Init] CAM:%s  MIC:%s  WiFi:%s  A9G:%s\n",
    camReady  ? "OK":"FAIL",
    micReady  ? "OK":"FAIL",
    wifiReady ? "OK":"FAIL",
    a9gReady  ? "OK":"FAIL");

  xTaskCreatePinnedToCore(TouchTask, "Touch", 4096, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(MainTask,  "Main",  8192, NULL, 2, NULL, 0);
  if (micReady)
    xTaskCreatePinnedToCore(VoiceWakeTask, "Voice", 8192, NULL, 1, NULL, 1);

  Serial.println("Ready. Double-tap / hold 2s / clap 3x to trigger SOS.");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));   // all work in FreeRTOS tasks
}
