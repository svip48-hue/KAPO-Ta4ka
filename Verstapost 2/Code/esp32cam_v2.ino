/*
  SBC-ESP32-CAM — Anduriplaat
  ============================
  Ülesanded:
    1. VL53L0X (ToF kaugus) — I2C 0x30
    2. TCS34725 (värvandur) — I2C 0x29
    3. QR-koodi skannimine potolokilt (ESP32QRCodeReader teek)
    4. Andmete saatmine WiFi kaudu XIAO ESP32-C3-le iga 200ms

  Vajalikud teegid (Library Manager / Add .ZIP):
    - ESP32QRCodeReader  → https://github.com/RuiSantosdotme/ESP32QRCodeReader/archive/refs/heads/master.zip
    - Adafruit VL53L0X  → Library Manager
    - Adafruit TCS34725 → Library Manager
    - Adafruit BusIO    → Library Manager (sõltuvus)
    - ArduinoJson       → Library Manager

  I2C pinnid (ESP32-CAM AI Thinker):
    SDA    → GPIO14
    SCL    → GPIO15
    XSHUT  → GPIO13  (VL53L0X aadressivahetus)

  Arduino IDE seaded:
    Board:  AI Thinker ESP32-CAM
    PSRAM:  Enabled  ← kohustuslik QR jaoks!
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <ESP32QRCodeReader.h>
//#include "Adafruit_VL53L0X.h"
//#include "Adafruit_TCS34725.h"

// ── WiFi ───────────────────────────────────────────────────
const char* WIFI_SSID = "RobotCar";
const char* WIFI_PASS = "12345678";
const char* MAIN_URL  = "http://192.168.4.1/sensor";

// ── Pinnid ─────────────────────────────────────────────────
//#define SDA_PIN    12
//#define SCL_PIN    16
//#define XSHUT_PIN  13

// ── QR lugeja ──────────────────────────────────────────────
ESP32QRCodeReader reader(CAMERA_MODEL_AI_THINKER);

// ── Andurid ────────────────────────────────────────────────
//Adafruit_VL53L0X  vl53;
//Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X);

//bool vl53_ok = false;
//bool tcs_ok  = false;

// ── Jagatud olek (QR task ↔ loop) ─────────────────────────
volatile char lastQR[250] = "";
SemaphoreHandle_t qrMutex;

// ── Intervallid ────────────────────────────────────────────
#define SEND_INTERVAL_MS  200

unsigned long lastSendMs = 0;

// ══════════════════════════════════════════════════════════
// QR TASK — jookseb taustal CPU core 1-l
// ══════════════════════════════════════════════════════════

void onQrCodeTask(void* pvParameters) {
  struct QRCodeData qrData;
  while (true) {
    if (reader.receiveQrCode(&qrData, 100)) {
      if (qrData.valid) {
        Serial.printf("QR leitud: %s\n", (const char*)qrData.payload);
        if (xSemaphoreTake(qrMutex, portMAX_DELAY)) {
          strncpy((char*)lastQR, (const char*)qrData.payload, sizeof(lastQR) - 1);
          xSemaphoreGive(qrMutex);
        }
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ══════════════════════════════════════════════════════════
// I2C ANDURITE INIT
// ══════════════════════════════════════════════════════════

// void initSensors() {
//   Wire.begin(SDA_PIN, SCL_PIN);

//   // Samm 1: Hoia VL53L0X väljas
//   pinMode(XSHUT_PIN, OUTPUT);
//   digitalWrite(XSHUT_PIN, LOW);
//   delay(10);

//   // Samm 2: Init TCS34725 (0x29) — VL53L0X magab
//   tcs_ok = tcs.begin(0x29, &Wire);
//   Serial.println(tcs_ok ? "TCS34725 OK" : "TCS34725 EI LEITUD!");

//   // Samm 3: Ärata VL53L0X
//   digitalWrite(XSHUT_PIN, HIGH);
//   delay(10);

//   // Samm 4: Init VL53L0X, muuda aadress 0x30
//   vl53_ok = vl53.begin(0x29, false, &Wire);
//   if (vl53_ok) {
//     vl53.setAddress(0x30);
//     Serial.println("VL53L0X OK → aadress muudetud 0x30");
//   } else {
//     Serial.println("VL53L0X EI LEITUD!");
//   }
// }

// ══════════════════════════════════════════════════════════
// ANDURITE LUGEMINE
// ══════════════════════════════════════════════════════════

// float readDistance() {
//   if (!vl53_ok) return -1;
//   VL53L0X_RangingMeasurementData_t m;
//   vl53.rangingTest(&m, false);
//   if (m.RangeStatus != 4) return m.RangeMilliMeter / 10.0;
//   return -1;
// }

// String readColor() {
//   if (!tcs_ok) return "unknown";
//   uint16_t r, g, b, c;
//   tcs.getRawData(&r, &g, &b, &c);
//   if (c == 0) return "unknown";

//   float rn = (float)r / c * 255;
//   float gn = (float)g / c * 255;
//   float bn = (float)b / c * 255;

//   if (c > 3000)                              return "white";
//   if (c < 500)                               return "black";
//   if (rn > gn * 1.5 && rn > bn * 1.5)       return "red";
//   if (bn > rn * 1.5 && bn > gn * 1.2)       return "blue";
//   if (gn > rn * 1.2 && gn > bn * 1.2)       return "green";
//   return "other";
// }

// ══════════════════════════════════════════════════════════
// ANDMETE SAATMINE
// ══════════════════════════════════════════════════════════

void sendData(float dist, String color) {
  if (WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<256> doc;
  doc["dist"]  = dist;
  doc["color"] = color;

  // Lisa QR kui on uus
  if (xSemaphoreTake(qrMutex, 10)) {
    if (strlen((char*)lastQR) > 0) {
      doc["qr"] = (char*)lastQR;
      lastQR[0] = '\0';  // tühjenda pärast saatmist
    }
    xSemaphoreGive(qrMutex);
  }

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(MAIN_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(150);  // lühike timeout et mitte blokeerida
  int code = http.POST(body);
  http.end();

  Serial.printf("→ dist=%.1fcm color=%s HTTP=%d\n", dist, color.c_str(), code);
}

// ══════════════════════════════════════════════════════════
// SETUP
// ══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32-CAM anduriplaat käivitub...");

  // Mutex QR andmete kaitsmiseks
  qrMutex = xSemaphoreCreateMutex();

  // I2C andurid
  initSensors();

  // QR lugeja — jookseb taustal core 1-l
  reader.setup();
  reader.beginOnCore(1);
  xTaskCreate(onQrCodeTask, "onQrCode", 4 * 1024, NULL, 4, NULL);
  Serial.println("QR lugeja käivitatud");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Ühendun WiFi-ga");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); Serial.print("."); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nÜhendatud! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi ebaõnnestus, taaskäivitan...");
    delay(2000); ESP.restart();
  }

  Serial.println("Valmis!");
}

// ══════════════════════════════════════════════════════════
// LOOP
// ══════════════════════════════════════════════════════════

void loop() {
  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    float  dist  = readDistance();
    String color = readColor();
    sendData(dist, color);
  }
}
