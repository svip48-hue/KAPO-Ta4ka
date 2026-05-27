/*
  SBC-ESP32-CAM — QR koodilugeja
  ================================
  Ainult QR koodide skannimine ja saatmine XIAO ESP32-C3-le.
  Dataureid ega muid andureid pole.

  Vajalikud teegid:
    - ESP32QRCodeReader → https://github.com/RuiSantosdotme/ESP32QRCodeReader/archive/refs/heads/master.zip
    - ArduinoJson       → Library Manager

  Arduino IDE seaded:
    Board:  AI Thinker ESP32-CAM
    PSRAM:  Enabled  ← kohustuslik!
    Partition: Huge APP (3MB No OTA)

  Laadimine:
    GPIO0 → GND enne laadimist
    Peale laadimist: eemalda GPIO0-GND, vajuta RESET
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32QRCodeReader.h>

// ── WiFi — ühendub XIAO peaplaadi AP-ga ───────────────────
const char* WIFI_SSID = "RobotCar";
const char* WIFI_PASS = "12345678";
const char* MAIN_URL  = "http://192.168.4.1/qr";

// ── QR lugeja ──────────────────────────────────────────────
ESP32QRCodeReader reader(CAMERA_MODEL_AI_THINKER);

// ── Viimane saadetud QR (et mitte sama saata korduvalt) ───
String lastSentQR = "";
unsigned long lastSentMs = 0;
#define QR_RESEND_INTERVAL 5000  // saada sama QR uuesti alles 5 sek pärast

// ── Mutex QR andmete kaitsmiseks ──────────────────────────
SemaphoreHandle_t qrMutex;
volatile char pendingQR[250] = "";
volatile bool hasNewQR = false;

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
          strncpy((char*)pendingQR, (const char*)qrData.payload, sizeof(pendingQR) - 1);
          hasNewQR = true;
          xSemaphoreGive(qrMutex);
        }
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ══════════════════════════════════════════════════════════
// SAATMINE XIAO-LE
// ══════════════════════════════════════════════════════════

void sendQR(const String& qrData) {
  if (WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<256> doc;
  doc["qr"] = qrData;
  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.begin(MAIN_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(200);
  int code = http.POST(body);
  http.end();

  Serial.printf("QR saadetud → HTTP %d | %s\n", code, qrData.c_str());
}

// ══════════════════════════════════════════════════════════
// SETUP
// ══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32-CAM QR lugeja käivitub...");

  // Mutex
  qrMutex = xSemaphoreCreateMutex();

  // QR lugeja käivitus
  reader.setup();
  reader.beginOnCore(1);
  xTaskCreate(onQrCodeTask, "onQrCode", 4 * 1024, NULL, 4, NULL);
  Serial.println("QR lugeja käivitatud (core 1)");

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Ühendun RobotCar WiFi-ga");

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nÜhendatud! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi ebaõnnestus — taaskäivitan...");
    delay(2000);
    ESP.restart();
  }

  Serial.println("Valmis! Otsin QR koode...");
}

// ══════════════════════════════════════════════════════════
// LOOP
// ══════════════════════════════════════════════════════════

void loop() {
  // Kontrolli kas on uus QR
  if (xSemaphoreTake(qrMutex, 10)) {
    if (hasNewQR) {
      String qr = String((char*)pendingQR);
      hasNewQR = false;
      xSemaphoreGive(qrMutex);

      // Saada kui see on uus QR või 5 sek on möödunud
      unsigned long now = millis();
      if (qr != lastSentQR || (now - lastSentMs) > QR_RESEND_INTERVAL) {
        sendQR(qr);
        lastSentQR = qr;
        lastSentMs = now;
      }
    } else {
      xSemaphoreGive(qrMutex);
    }
  }

  delay(10);
}
