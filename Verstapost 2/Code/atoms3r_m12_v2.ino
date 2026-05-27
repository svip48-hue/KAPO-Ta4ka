/*
  AtomS3R M12 — MJPEG стрим + VL53L0X кaugusmõõtmine
  =====================================================
  - Камера OV3660 стримит на /stream
  - VL53L0X подключён к PORT A (G1=SDA, G2=SCL)
  - Данные дальности отправляются на XIAO ESP32-C3 каждые 200мс
  - Браузер берёт видео напрямую отсюда

  Arduino IDE:
    Board: M5Stack AtomS3R
    PSRAM: OPI PSRAM
    Partition: Huge APP (3MB No OTA)
    USB CDC On Boot: Enabled

  Библиотеки:
    Adafruit VL53L0X
    Adafruit BusIO
    ArduinoJson
*/

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "Adafruit_VL53L0X.h"

// ── WiFi ───────────────────────────────────────────────────
const char* WIFI_SSID = "RobotCar";
const char* WIFI_PASS = "12345678";
const char* MAIN_URL  = "http://192.168.4.1/sensor";  // XIAO

// ── PORT A пины (I2C для VL53L0X) ─────────────────────────
#define PORT_A_SDA  1   // G1
#define PORT_A_SCL  2   // G2

// ── VL53L0X ───────────────────────────────────────────────
Adafruit_VL53L0X vl53;
bool vl53_ok = false;
unsigned long lastSendMs = 0;
#define SEND_INTERVAL_MS 350

// ── Пины камеры AtomS3R M12 (OV3660, 3MP) ─────────────────
// GPIO18 LOW = включить питание камеры (обязательно до init!)
// GPIO38 = аппаратный reset камеры
#define CAM_POWER_PIN   18
#define CAM_RESET_PIN   38

#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  38
#define XCLK_GPIO_NUM   21
#define SIOD_GPIO_NUM   12
#define SIOC_GPIO_NUM    9
#define Y9_GPIO_NUM     13
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     17
#define Y6_GPIO_NUM      4
#define Y5_GPIO_NUM     48
#define Y4_GPIO_NUM     46
#define Y3_GPIO_NUM     42
#define Y2_GPIO_NUM      3
#define VSYNC_GPIO_NUM  10
#define HREF_GPIO_NUM   14
#define PCLK_GPIO_NUM   40

WebServer server(80);

// ── MJPEG стрим ───────────────────────────────────────────
void handleStream() {
  WiFiClient client = server.client();

  // Заголовок multipart
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  client.print(response);

  while (client.connected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      break;
    }

    // Отправляем один кадр
    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    client.write(fb->buf, fb->len);
    client.print("\r\n");

    esp_camera_fb_return(fb);

    // Небольшая пауза — ~15 fps
    delay(66);
  }
}

// ── Одиночный снимок (для отладки) ────────────────────────
void handleCapture() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Camera error");
    return;
  }
  server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
  server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

// ── Статус ─────────────────────────────────────────────────
void handleStatus() {
  server.send(200, "text/plain", "AtomS3R camera OK");
}

void setup() {
  Serial.begin(115200);
  Serial.println("AtomS3R Camera starting...");

  // ── Питание камеры OV3660 ──────────────────────────────
  // GPIO18 LOW — включить питание I2C и камеры (M12 специфика)
  // Без этого камера не инициализируется и I2C не работает
  pinMode(CAM_POWER_PIN, OUTPUT);
  digitalWrite(CAM_POWER_PIN, LOW);
  delay(100);

  // GPIO38 — аппаратный reset камеры
  pinMode(CAM_RESET_PIN, OUTPUT);
  digitalWrite(CAM_RESET_PIN, LOW);   // reset активен
  delay(100);
  digitalWrite(CAM_RESET_PIN, HIGH);  // отпустить reset
  delay(1500);                        // дать камере стабилизироваться

  // ── Инициализация камеры ───────────────────────────────
  camera_config_t config;
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = Y2_GPIO_NUM;
  config.pin_d1        = Y3_GPIO_NUM;
  config.pin_d2        = Y4_GPIO_NUM;
  config.pin_d3        = Y5_GPIO_NUM;
  config.pin_d4        = Y6_GPIO_NUM;
  config.pin_d5        = Y7_GPIO_NUM;
  config.pin_d6        = Y8_GPIO_NUM;
  config.pin_d7        = Y9_GPIO_NUM;
  config.pin_xclk      = XCLK_GPIO_NUM;
  config.pin_pclk      = PCLK_GPIO_NUM;
  config.pin_vsync     = VSYNC_GPIO_NUM;
  config.pin_href      = HREF_GPIO_NUM;
  config.pin_sccb_sda  = SIOD_GPIO_NUM;
  config.pin_sccb_scl  = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 20000000;
  config.pixel_format  = PIXFORMAT_JPEG;
  config.grab_mode     = CAMERA_GRAB_LATEST;

  // OV3660: 3MP, 8MB PSRAM на борту
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_HD;    // 1280x720 — хороший баланс скорости и качества
    config.jpeg_quality = 10;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_VGA;
    config.jpeg_quality = 15;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    delay(3000);
    ESP.restart();
  }
  Serial.println("Camera OK");

  // Дополнительные настройки сенсора
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_hmirror(s, 0);   // зеркало по горизонтали: 1 = да
    s->set_vflip(s, 0);     // перевернуть: 1 = да
  }

  // ── Подключение к WiFi ─────────────────────────────────
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to RobotCar WiFi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected! IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("Stream: http://%s/stream\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi failed, restarting...");
    delay(3000);
    ESP.restart();
  }

  // ── VL53L0X на PORT A (G1=SDA, G2=SCL) ───────────────
  Wire.begin(PORT_A_SDA, PORT_A_SCL);
  vl53_ok = vl53.begin(0x29, false, &Wire);
  if (vl53_ok) {
    
    Serial.println("VL53L0X OK (PORT A)");
  } else {
    Serial.println("VL53L0X ei leitud PORT A-l!");
  }

  // ── HTTP маршруты ──────────────────────────────────────
  server.on("/stream",  handleStream);
  server.on("/capture", handleCapture);
  server.on("/status",  handleStatus);
  server.begin();

  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();

  // ── Saada kaugusandmed XIAO-le iga 200ms ──────────────
  if (vl53_ok && millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    VL53L0X_RangingMeasurementData_t m;
    vl53.rangingTest(&m, false);
    delay(50);

    if (m.RangeStatus != 4) {
      float dist = m.RangeMilliMeter / 10.0;

      if (WiFi.status() == WL_CONNECTED) {
        StaticJsonDocument<64> doc;
        doc["dist"] = dist;
        String body;
        serializeJson(doc, body);

        HTTPClient http;
        http.begin(MAIN_URL);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(150);
        http.POST(body);
        http.end();

        Serial.printf("Dist: %.1f cm\n", dist);
      }
    }
  }
}
