/*
  M5Atom Lite – VL53L0X дальномер
  Отправляет данные на главный ESP32 (RobotCar) при наличии WiFi.
  Если WiFi нет – измеряет и выводит в Serial.
  WiFi: RobotCar / 12345678
  URL: http://192.168.4.1/sensor
*/

#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_VL53L0X.h>

// Пины I2C для M5Atom Lite
#define I2C_SDA 25
#define I2C_SCL 21

// WiFi настройки (точка доступа главного ESP32)
const char* WIFI_SSID = "RobotCar";
const char* WIFI_PASS = "12345678";
const char* MAIN_URL  = "http://192.168.4.1/sensor";

// Датчик
Adafruit_VL53L0X vl53;
bool vl53_ok = false;

// Таймер измерения и отправки
unsigned long lastMeasureMs = 0;
const unsigned long MEASURE_INTERVAL_MS = 250; // 4 раза в секунду

// Последнее измеренное расстояние (для отправки)
float last_distance_cm = 0.0;
bool last_valid = false;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nM5Atom Lite VL53L0X sender (works without WiFi)");

  // 1. Инициализация I2C и датчика
  Wire.begin(I2C_SDA, I2C_SCL);
  vl53_ok = vl53.begin();
  if (vl53_ok) {
    Serial.println("VL53L0X found");
    // Опционально: настройка режима (HIGH_ACCURACY или LONG_RANGE)
    vl53.configSensor(Adafruit_VL53L0X::VL53L0X_SENSE_LONG_RANGE);
  } else {
    Serial.println("VL53L0X not found!");
  }

  // 2. Подключение к WiFi (RobotCar) – не блокируем, просто пытаемся
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to RobotCar");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi not available, will keep measuring locally");
  }
}

void loop() {
  // Измеряем расстояние с заданным интервалом (всегда)
  unsigned long now = millis();
  if (vl53_ok && (now - lastMeasureMs >= MEASURE_INTERVAL_MS)) {
    lastMeasureMs = now;

    // Получаем данные с датчика
    VL53L0X_RangingMeasurementData_t measure;
    vl53.rangingTest(&measure, false); // false = однократное измерение

    if (measure.RangeStatus != 4) { // 4 = out of range
      last_distance_cm = measure.RangeMilliMeter / 10.0;
      last_valid = true;
      Serial.printf("Measured: %.1f cm\n", last_distance_cm);
    } else {
      last_valid = false;
      Serial.println("Out of range");
    }
  }

  // Отправляем последнее корректное значение на сервер, если WiFi подключён
  if (WiFi.status() == WL_CONNECTED && last_valid) {
    // Можно отправлять с тем же интервалом, что и измерения
    // (или отдельным таймером – здесь отправляем при каждом измерении)
    HTTPClient http;
    http.begin(MAIN_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(150);

    StaticJsonDocument<64> doc;
    doc["dist"] = last_distance_cm;
    String body;
    serializeJson(doc, body);

    int httpCode = http.POST(body);
    http.end();

    if (httpCode == 200) {
      Serial.printf("Sent: %.1f cm\n", last_distance_cm);
    } else {
      Serial.printf("HTTP error %d\n", httpCode);
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    // Периодически пытаемся восстановить WiFi (без задержки, мешающей измерениям)
    static unsigned long lastReconnectAttempt = 0;
    if (millis() - lastReconnectAttempt > 5000) { // раз в 5 секунд
      lastReconnectAttempt = millis();
      WiFi.reconnect();
      Serial.println("Trying to reconnect WiFi...");
    }
  }

  // Небольшая задержка для стабильности цикла
  delay(10);
}