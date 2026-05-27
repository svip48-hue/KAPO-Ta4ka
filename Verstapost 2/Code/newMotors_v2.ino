/*
  Robot Car v3 — XIAO ESP32-C3 + 2x DRI0044 (TB6612)
  Оптимизации:
    - WebSocket вместо HTTP для команд (задержка 1-5мс вместо 20-50мс)
    - Автостоп если соединение оборвалось (таймаут 300мс)
    - Кэширование HTML страницы в браузере
    - Камера AtomS3R стримит напрямую в браузер

  Библиотека: ArduinoWebsockets by Gil Maimon
  Установить: Arduino IDE → Library Manager → "WebSockets" by Markus Sattler
              или "ArduinoWebsockets" by Gil Maimon

  Пины DRI0044 #1 (левый борт):
    PWM1 → GPIO2 (D0)
    DIR1 → GPIO3 (D1)
    PWM2 → GPIO4 (D2)
    DIR2 → GPIO5 (D3)
    VM   → 7.4V (батарея)
    GND  → GND
    VCC  → 3.3V

  Пины DRI0044 #2 (правый борт):
    PWM1 → GPIO6 (D4)
    DIR1 → GPIO7 (D5)
    PWM2 → GPIO21 (D6)
    DIR2 → GPIO20 (D7)
    VM   → 7.4V
    GND  → GND
    VCC  → 3.3V
*/  

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include "Adafruit_TCS34725.h"

// ── Пины датчика цвета ─────────────────────────────────────
#define SDA_PIN    8
#define SCL_PIN    9

// ── TCS34725 ───────────────────────────────────────────────
Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS34725_INTEGRATIONTIME_50MS, TCS34725_GAIN_4X);
bool tcs_ok = false;

// ── Данные датчиков ────────────────────────────────────────
float  sensorDist  = -1;   // приходит от AtomS3R по /sensor
String sensorColor = "unknown";
unsigned long lastSensorMs = 0;
#define SENSOR_INTERVAL_MS 200


// Для теста латентности
volatile uint32_t lastCmdReceiveTime = 0;
volatile uint32_t lastMotorApplyTime = 0;
volatile bool latencyMeasurementActive = false;


// Для отладки датчика цвета
unsigned long lastColorPrintMs = 0;
const unsigned long COLOR_PRINT_INTERVAL_MS = 500;

// ── WiFi AP ────────────────────────────────────────────────
const char* AP_SSID = "RobotCar";
const char* AP_PASS = "12345678";

// ── Пины DRI0044 #1 (левый борт) ───────────────────────────
#define L_PWM1  2   // D0 - левый мотор PWM
#define L_DIR1  3   // D1 - левый мотор DIR
#define L_PWM2  4   // D2 - правый мотор PWM (на левом драйвере)
#define L_DIR2  5   // D3 - правый мотор DIR (на левом драйвере)

// ── Пины DRI0044 #2 (правый борт) ──────────────────────────
#define R_PWM1  6   // D4 - левый мотор PWM (на правом драйвере)
#define R_DIR1  7   // D5 - левый мотор DIR (на правом драйвере)
#define R_PWM2  21  // D6 - правый мотор PWM (на правом драйвере)
#define R_DIR2  20  // D7 - правый мотор DIR (на правом драйвере)

// ── Параметры ──────────────────────────────────────────────
#define MAX_SPEED       255
#define MIN_SPEED       50     // минимальная скорость для движения
#define WS_TIMEOUT_MS   2000   // мс — автостоп если нет команды

// ── Состояние моторов ──────────────────────────────────────
enum MotorDir { FORWARD, BACKWARD, STOPPED };

volatile int leftSpeed = 0;
volatile int rightSpeed = 0;
volatile MotorDir leftDir = STOPPED;
volatile MotorDir rightDir = STOPPED;

// Глобальная скорость (ОБЪЯВЛЯЕМ ДО ИСПОЛЬЗОВАНИЯ)
volatile int currentSpeed = 180;  // ~70% от максимума

// Автостоп
unsigned long lastCmdMs = 0;
bool autoStopped = false;

// ── Серверы ────────────────────────────────────────────────
WebServer       httpServer(80);
WebSocketsServer wsServer(81);

// ══════════════════════════════════════════════════════════
// МОТОРЫ (DRI0044 / TB6612)
// ══════════════════════════════════════════════════════════

// Применяем скорость и направление к одному мотору
void setMotor(int pwmPin, int dirPin, MotorDir dir, int speed) {
  // Ограничение скорости
  if (speed < 0) speed = 0;
  if (speed > MAX_SPEED) speed = MAX_SPEED;
  
  // Установка направления и скорости
  switch (dir) {
    case FORWARD:
      digitalWrite(dirPin, HIGH);
      analogWrite(pwmPin, speed);
      break;
    case BACKWARD:
      digitalWrite(dirPin, LOW);
      analogWrite(pwmPin, speed);
      break;
    case STOPPED:
    default:
      analogWrite(pwmPin, 0);
      digitalWrite(dirPin, LOW);  // не важно, но для порядка
      break;
  }
}

// Применяем ко всем 4 моторам
void applyMotors() {
  // Левый драйвер управляет двумя левыми колёсами (если у вас 4WD)
  // Если у вас 2WD, то используем только PWM1 на каждом драйвере
  setMotor(L_PWM1, L_DIR1, leftDir, leftSpeed);
  setMotor(L_PWM2, L_DIR2, leftDir, leftSpeed);   // второе колесо левого борта
  
  setMotor(R_PWM1, R_DIR1, rightDir, rightSpeed);
  setMotor(R_PWM2, R_DIR2, rightDir, rightSpeed);
}

// Установка направления и скорости
void setMotors(MotorDir left, MotorDir right, int lSpd, int rSpd) {
  leftDir = left;
  rightDir = right;
  leftSpeed = lSpd;
  rightSpeed = rSpd;
  
  // Если скорость слишком мала для движения — останавливаем
  if (lSpd < MIN_SPEED && left != STOPPED) {
    leftDir = STOPPED;
    leftSpeed = 0;
  }
  if (rSpd < MIN_SPEED && right != STOPPED) {
    rightDir = STOPPED;
    rightSpeed = 0;
  }
  
  applyMotors();
  
  Serial.printf("Motors: L=%d (%d) R=%d (%d)\n", 
                leftDir, leftSpeed, rightDir, rightSpeed);
}

// Перегрузка для обратной совместимости (только направление)
void setMotors(MotorDir left, MotorDir right) {
  int lSpd = (left != STOPPED) ? currentSpeed : 0;
  int rSpd = (right != STOPPED) ? currentSpeed : 0;
  setMotors(left, right, lSpd, rSpd);
}

void setSpeed(int speed) {
  if (speed < MIN_SPEED) speed = MIN_SPEED;
  if (speed > MAX_SPEED) speed = MAX_SPEED;
  
  currentSpeed = speed;
  
  // Обновляем скорость только если моторы в движении
  if (leftDir != STOPPED || rightDir != STOPPED) {
    leftSpeed = (leftDir != STOPPED) ? currentSpeed : 0;
    rightSpeed = (rightDir != STOPPED) ? currentSpeed : 0;
    applyMotors();
  }
  
  Serial.printf("Speed set to: %d\n", currentSpeed);
}

// ══════════════════════════════════════════════════════════
// ДАТЧИКИ
// ══════════════════════════════════════════════════════════

void initSensors() {
  Wire.begin(SDA_PIN, SCL_PIN);

  // TCS34725 — единственный датчик на шине, адрес 0x29
  tcs_ok = tcs.begin(0x29, &Wire);
  Serial.println(tcs_ok ? "TCS34725 OK" : "TCS34725 не найден!");
}

float readDistance() {
  return sensorDist;  // приходит от AtomS3R через /sensor
}

String readColor() {
  if (!tcs_ok) return "unknown";
  uint16_t r, g, b, c;
  tcs.getRawData(&r, &g, &b, &c);
  if (c == 0) return "unknown";
  if (c > 3000)                        return "white";
  if (c < 500)                         return "black";
  float rn = (float)r/c*255, gn = (float)g/c*255, bn = (float)b/c*255;
  if (rn > gn*1.5 && rn > bn*1.5)     return "red";
  if (bn > rn*1.5 && bn > gn*1.2)     return "blue";
  if (gn > rn*1.2 && gn > bn*1.2)     return "green";
  return "other";
}

void updateSensors() {
  if (millis() - lastSensorMs < SENSOR_INTERVAL_MS) return;
  lastSensorMs = millis();

  // Цвет читаем локально
  sensorColor = readColor();

  // Расстояние приходит от AtomS3R через POST /sensor
  // Отправить всем браузерам через WebSocket
  StaticJsonDocument<128> doc;
  doc["type"]  = "sensors";
  doc["dist"]  = sensorDist;
  doc["color"] = sensorColor;
  String msg;
  serializeJson(doc, msg);
  wsServer.broadcastTXT(msg);
}

void applyCommand(const String& cmd) {
  lastCmdMs = millis();
  autoStopped = false;

   // ---- НАЧАЛО ИЗМЕРЕНИЯ ЛАТЕНТНОСТИ (для команды LAT) ----
    uint32_t t_receive = micros();  // точное время получения
    bool measureLatency = (cmd == "LAT");
    if (measureLatency) {
        lastCmdReceiveTime = t_receive;
    }

  if (cmd == "F") {
    setMotors(FORWARD, FORWARD, currentSpeed, currentSpeed);
  }
  else if (cmd == "B") {
    setMotors(BACKWARD, BACKWARD, currentSpeed, currentSpeed);
  }
  else if (cmd == "L") {
    setMotors(FORWARD, BACKWARD, currentSpeed, currentSpeed);  // разворот на месте
  }
  else if (cmd == "R") {
    setMotors(BACKWARD, FORWARD, currentSpeed, currentSpeed);  // разворот на месте
  }
  else if (cmd == "S") {
    setMotors(STOPPED, STOPPED, 0, 0);
  }
  else if (cmd.startsWith("V")) {
    uint32_t v = cmd.substring(1).toInt();
    // Маппинг: слайдер 50..2000 → скорость MIN_SPEED..MAX_SPEED
    int mappedSpeed = map(v, 50, 2000, MIN_SPEED, MAX_SPEED);
    setSpeed(mappedSpeed);
  }
   // ---- ДОБАВЛЯЕМ ОБРАБОТКУ LAT ----
    else if (cmd == "LAT") {
        // Ничего не двигаем, просто замерим задержку от получения команды до отправки ответа
        // (чисто сетевая + обработка). Для измерения задержки до мотора используйте LAT_MOTOR.
        uint32_t t_after = micros();
        uint32_t latency_us = t_after - lastCmdReceiveTime;
        // Отправляем результат обратно по WebSocket (клиент получит и покажет)
        String reply = "LAT_RESULT " + String(latency_us) + " us";
        wsServer.broadcastTXT(reply);
        Serial.printf("Latency (cmd->reply): %u us\n", latency_us);
    }
    else if (cmd == "LAT_MOTOR") {
        // Измеряем задержку от получения команды до физического изменения моторов
        // Сначала принудительно применяем краткое движение вперёд на 10 мс, чтобы моторы точно изменили состояние
        setMotors(FORWARD, FORWARD, MIN_SPEED, MIN_SPEED);
        uint32_t t_apply = micros();
        uint32_t latency_us = t_apply - lastCmdReceiveTime;
        // Возвращаем моторы в стоп
        delay(10);
        setMotors(STOPPED, STOPPED, 0, 0);
        // Отправляем результат
        String reply = "MOTOR_LATENCY " + String(latency_us) + " us";
        wsServer.broadcastTXT(reply);
        Serial.printf("Motor latency (cmd->apply): %u us\n", latency_us);
    }

    // ---- ЗАПОМИНАЕМ ВРЕМЯ ПРИМЕНЕНИЯ (для будущих измерений) ----
    if (measureLatency) {
        lastMotorApplyTime = micros();
    }
}

// ══════════════════════════════════════════════════════════
// WEBSOCKET
// ══════════════════════════════════════════════════════════

void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[WS] Client %u connected\n", num);
      wsServer.sendTXT(num, "OK");
      break;

    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client %u disconnected → autostop\n", num);
      setMotors(STOPPED, STOPPED, 0, 0);
      break;

    case WStype_TEXT: {
      String msg = String((char*)payload);
      msg.trim();
      applyCommand(msg);
      break;
    }
    case WStype_TEXT: {
    String msg = String((char*)payload);
    msg.trim();
    
    // ----- ПИНГ ДЛЯ ИЗМЕРЕНИЯ RTT -----
    if (msg.startsWith("PING")) {
        // Формат: PING<число>
        uint32_t id = msg.substring(4).toInt();
        wsServer.sendTXT(num, "PONG" + String(id));
        break;
    }
    // ----- ОБЫЧНЫЕ КОМАНДЫ ДВИЖЕНИЯ -----
    applyCommand(msg);
    break;
}

    default: break;
  }
}

// ══════════════════════════════════════════════════════════
// HTML (без изменений)
// ══════════════════════════════════════════════════════════

const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, user-scalable=no">
<title>Robot Car</title>
<style>
  :root {
    --bg:#0f0f13; --surface:#1a1a24; --border:rgba(255,255,255,0.08);
    --accent:#4f8ef7; --accent2:#7c5cfc; --text:#e8e8f0; --muted:#7070a0;
    --btn-bg:#252535; --btn-hover:#303048; --danger:#f75f4f; --success:#4fcc8e;
  }
  *{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
  body{
    background:var(--bg);color:var(--text);
    font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
    min-height:100dvh;display:flex;flex-direction:column;
    align-items:center;justify-content:center;gap:16px;padding:16px;
    user-select:none;
  }
  h1{
    font-size:20px;font-weight:600;
    background:linear-gradient(135deg,var(--accent),var(--accent2));
    -webkit-background-clip:text;-webkit-text-fill-color:transparent;
  }
  /* status */
  .sbar{display:flex;gap:10px;align-items:center;font-size:12px;color:var(--muted)}
  .sdot{width:8px;height:8px;border-radius:50%;background:#555;transition:background .3s}
  .sdot.ok{background:var(--success);animation:pulse 2s infinite}
  .sdot.err{background:var(--danger)}
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
  .slabel{transition:color .3s}

  /* камера */
  .cam-section{width:100%;max-width:480px;display:flex;flex-direction:column;gap:6px}
  .cam-header{display:flex;justify-content:space-between;align-items:center;font-size:12px;color:var(--muted)}
  .cam-toggle{background:var(--btn-bg);border:1px solid var(--border);border-radius:8px;
    color:var(--text);font-size:11px;padding:3px 10px;cursor:pointer}
  .cam-toggle.on{background:var(--accent);border-color:var(--accent);color:#fff}
  .cam-ip{flex:1;background:var(--surface);border:1px solid var(--border);border-radius:8px;
    color:var(--text);font-size:12px;padding:4px 8px;outline:none;width:100%}
  .cam-box{width:100%;aspect-ratio:4/3;background:#0a0a10;border:1px solid var(--border);
    border-radius:12px;overflow:hidden;display:flex;align-items:center;justify-content:center;position:relative}
  .cam-box img{width:100%;height:100%;object-fit:cover;display:none;transform: rotate(270deg)}
  .cam-box img.on{display:block}
  .cam-ph{color:var(--muted);font-size:12px;text-align:center;padding:16px;line-height:1.8}
  .live-badge{position:absolute;top:8px;right:8px;background:rgba(0,0,0,.55);
    border-radius:6px;padding:2px 8px;font-size:11px;color:var(--success);display:none}
  .live-badge.on{display:block}

  /* dpad */
  .dpad{display:grid;grid-template-columns:repeat(3,80px);grid-template-rows:repeat(3,80px);gap:8px}
  .btn{background:var(--btn-bg);border:1px solid var(--border);border-radius:16px;
    color:var(--text);font-size:26px;cursor:pointer;display:flex;
    align-items:center;justify-content:center;
    transition:background .08s,transform .08s,box-shadow .08s;touch-action:none}
  .btn:hover{background:var(--btn-hover)}
  .btn.pressed{background:var(--accent);border-color:var(--accent);
    transform:scale(.92);box-shadow:0 0 14px rgba(79,142,247,.45)}
  .btn.stop-btn.pressed{background:var(--danger);border-color:var(--danger);
    box-shadow:0 0 14px rgba(247,95,79,.45)}
  .btn.empty{visibility:hidden;pointer-events:none}

  /* скорость */
  .spd{width:100%;max-width:280px;display:flex;flex-direction:column;gap:8px}
  .spd-lbl{display:flex;justify-content:space-between;font-size:12px;color:var(--muted)}
  .spd-lbl span{color:var(--text);font-weight:500}
  input[type=range]{width:100%;height:6px;-webkit-appearance:none;
    background:var(--surface);border-radius:3px;outline:none;border:1px solid var(--border)}
  input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;
    border-radius:50%;background:var(--accent);cursor:pointer;
    box-shadow:0 0 8px rgba(79,142,247,.5)}

  .cur{font-size:12px;color:var(--muted);text-align:center}
  .cur span{color:var(--accent);font-weight:500}

  /* датчики */
  .sensors{
    width:100%;max-width:320px;
    display:grid;grid-template-columns:1fr 1fr;gap:8px;
  }
  .sensor-card{
    background:var(--surface);border:1px solid var(--border);
    border-radius:12px;padding:10px 14px;
    display:flex;flex-direction:column;gap:4px;
  }
  .sensor-label{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:.5px}
  .sensor-value{font-size:20px;font-weight:600;color:var(--text)}
  .sensor-unit{font-size:10px;color:var(--muted)}
  .color-dot{
    width:16px;height:16px;border-radius:50%;
    display:inline-block;margin-right:6px;vertical-align:middle;
    border:1px solid var(--border);
  }
</style>
</head>
<body>
<h1>&#x1F916; Robot Car</h1>

<div class="sbar">
  <div class="sdot" id="sdot"></div>
  <span class="slabel" id="slabel">Подключение...</span>
</div>

<div class="cam-section">
  <div class="cam-header">
    <span>&#x1F4F7; Камера (AtomS3R)</span>
    <button class="cam-toggle" id="cam-btn" onclick="toggleCam()">Включить</button>
  </div>
  <input class="cam-ip" type="text" id="cam-ip" value="192.168.4.2" placeholder="IP камеры">
  <div class="cam-box">
    <div class="cam-ph" id="cam-ph">Нажми "Включить"<br>чтобы открыть стрим</div>
    <img id="cam-img" alt="stream" onload="camOk()" onerror="camFail()">
    <div class="live-badge" id="live-badge">&#x25CF; LIVE</div>
  </div>
</div>

<div class="sensors">
  <div class="sensor-card">
    <div class="sensor-label">&#x1F50D; Kaugus</div>
    <div class="sensor-value" id="s-dist">—</div>
    <div class="sensor-unit">cm</div>
  </div>
  <div class="sensor-card">
    <div class="sensor-label">&#x1F3A8; Värv</div>
    <div class="sensor-value" id="s-color">
      <span class="color-dot" id="s-dot" style="background:#555"></span>
      <span id="s-colorname">—</span>
    </div>
  </div>
</div>

<div class="dpad">
  <div class="btn empty"></div>
  <div class="btn" data-cmd="F">&#9650;</div>
  <div class="btn empty"></div>
  <div class="btn" data-cmd="L">&#9664;</div>
  <div class="btn stop-btn" data-cmd="S">&#9632;</div>
  <div class="btn" data-cmd="R">&#9654;</div>
  <div class="btn empty"></div>
  <div class="btn" data-cmd="B">&#9660;</div>
  <div class="btn empty"></div>
</div>

<div class="spd">
  <div class="spd-lbl">Скорость <span id="spd-val">50%</span></div>
  <input type="range" id="spd-slider" min="50" max="2000" value="400" step="50">
</div>
<div class="latency-test">
  <button id="test-latency-btn" class="cam-toggle" style="background:var(--accent);">📊 Тест задержки (100 команд)</button>
  <div id="latency-stats" style="font-size:12px; margin-top:10px; background:var(--surface); border-radius:12px; padding:10px; display:none;">
    <div style="font-weight:bold; margin-bottom:6px;">📈 Результаты (мс)</div>
    <div>Минимум: <span id="lat-min">—</span></div>
    <div>Медиана: <span id="lat-med">—</span></div>
    <div>Максимум: <span id="lat-max">—</span></div>
    <div style="margin-top:6px;">⏱ Задержка мотора (ESP32):<br> минимум <span id="mot-min">—</span> мс / медиана <span id="mot-med">—</span> / макс <span id="mot-max">—</span></div>
    <div style="margin-top:4px; color:var(--muted); font-size:10px;">(измерено по 100 командам LAT_MOTOR)</div>
  </div>
</div>

<div class="cur">Команда: <span id="cur-cmd">стоп</span></div>

<script>
// ── WebSocket ───────────────────────────────────────────────
const WS_PORT = 81;
let ws, wsReady = false;
const cmdNames = {F:'вперёд',B:'назад',L:'влево',R:'вправо',S:'стоп'};

async function runLatencyTest() {
  if (testActive || !wsReady || ws.readyState !== WebSocket.OPEN) {
    alert('Нет соединения');
    return;
  }
  testActive = true;
  testBtn.disabled = true;
  testBtn.textContent = 'Тест RTT... 0/100';
  statsDiv.style.display = 'block';

  const N = 100;
  let rttResults = [];

  // 1. Измеряем RTT через PING
  for (let i = 0; i < N; i++) {
    const id = Date.now() + i + Math.random();
    const start = performance.now();
    ws.send('PING' + id);
    const rtt = await new Promise((resolve) => {
      const handler = (e) => {
        if (e.data === 'PONG' + id) {
          ws.removeEventListener('message', handler);
          resolve(performance.now() - start);
        }
      };
      ws.addEventListener('message', handler);
      // таймаут на всякий случай
      setTimeout(() => { ws.removeEventListener('message', handler); resolve(null); }, 1000);
    });
    if (rtt !== null) rttResults.push(rtt);
    testBtn.textContent = `Тест RTT... ${i+1}/${N}`;
    await new Promise(r => setTimeout(r, 20)); // небольшая пауза
  }

  // 2. Измеряем задержку мотора (LAT_MOTOR)
  testBtn.textContent = 'Замер мотора... 0/100';
  let motorLatencies = [];

  for (let i = 0; i < N; i++) {
    ws.send('LAT_MOTOR');
    const motorUs = await new Promise((resolve) => {
      const handler = (e) => {
        if (typeof e.data === 'string' && e.data.startsWith('MOTOR_LATENCY')) {
          const us = parseInt(e.data.split(' ')[1]);
          ws.removeEventListener('message', handler);
          resolve(us);
        }
      };
      ws.addEventListener('message', handler);
      setTimeout(() => { ws.removeEventListener('message', handler); resolve(null); }, 200);
    });
    if (motorUs !== null) motorLatencies.push(motorUs / 1000); // в мс
    testBtn.textContent = `Замер мотора... ${i+1}/${N}`;
    await new Promise(r => setTimeout(r, 20));
  }

  // Статистика RTT
  if (rttResults.length) {
    rttResults.sort((a,b)=>a-b);
    const minRtt = rttResults[0];
    const maxRtt = rttResults[rttResults.length-1];
    const medianRtt = rttResults[Math.floor(rttResults.length/2)];
    document.getElementById('lat-min').innerText = minRtt.toFixed(2);
    document.getElementById('lat-med').innerText = medianRtt.toFixed(2);
    document.getElementById('lat-max').innerText = maxRtt.toFixed(2);
  } else {
    document.getElementById('lat-min').innerText = 'ошибка';
  }

  // Статистика мотора
  if (motorLatencies.length) {
    motorLatencies.sort((a,b)=>a-b);
    document.getElementById('mot-min').innerText = motorLatencies[0].toFixed(3);
    document.getElementById('mot-med').innerText = motorLatencies[Math.floor(motorLatencies.length/2)].toFixed(3);
    document.getElementById('mot-max').innerText = motorLatencies[motorLatencies.length-1].toFixed(3);
  } else {
    document.getElementById('mot-min').innerText = '—';
  }

  testBtn.disabled = false;
  testBtn.textContent = '📊 Тест задержки (100 команд)';
  testActive = false;
  alert('Тест завершён. Результаты обновлены.');
}

// Привязываем кнопку
testBtn.onclick = runLatencyTest;

function connect() {
  ws = new WebSocket('ws://' + location.hostname + ':' + WS_PORT + '/');
  ws.onopen = () => {
    wsReady = true;
    dot(true);
  };
  ws.onmessage = e => {
    try {
      const d = JSON.parse(e.data);
      if (d.type === 'sensors') updateSensorUI(d);
    } catch {}
  };
  ws.onclose = () => {
    wsReady = false;
    dot(false);
    // Переподключение через 1с
    setTimeout(connect, 1000);
  };
  ws.onerror = () => ws.close();
}

function dot(ok) {
  document.getElementById('sdot').className   = 'sdot ' + (ok ? 'ok' : 'err');
  document.getElementById('slabel').textContent = ok ? 'Подключено (WS)' : 'Переподключение...';
}

function send(msg) {
  if (wsReady && ws.readyState === WebSocket.OPEN) ws.send(msg);
}

// ── Команды ────────────────────────────────────────────────
function sendCmd(cmd) {
  send(cmd);
  document.getElementById('cur-cmd').textContent = cmdNames[cmd] || cmd;
}

// ── D-pad ──────────────────────────────────────────────────
document.querySelectorAll('.btn[data-cmd]').forEach(btn => {
  const cmd = btn.dataset.cmd;
let holdInterval = null;

const press = () => {
  btn.classList.add('pressed');
  sendCmd(cmd);
  // Повторять команду каждые 200мс пока держишь
  holdInterval = setInterval(() => sendCmd(cmd), 200);
};
const release = () => {
  btn.classList.remove('pressed');
  clearInterval(holdInterval);
  holdInterval = null;
  if (cmd !== 'S') sendCmd('S');
};

  btn.addEventListener('mousedown',  press);
  btn.addEventListener('mouseup',    release);
  btn.addEventListener('mouseleave', e => { if (e.buttons) release(); });
  btn.addEventListener('touchstart', e => { e.preventDefault(); press(); },   {passive:false});
  btn.addEventListener('touchend',   e => { e.preventDefault(); release(); }, {passive:false});
  btn.addEventListener('touchcancel',e => { e.preventDefault(); release(); }, {passive:false});
});

// ── Клавиатура ─────────────────────────────────────────────
const keyMap = {ArrowUp:'F',ArrowDown:'B',ArrowLeft:'L',ArrowRight:'R',' ':'S',w:'F',s:'B',a:'L',d:'R'};
const held = new Set();
document.addEventListener('keydown', e => {
  
  held.add(e.key);
  const c = keyMap[e.key];
  if (c) { e.preventDefault(); sendCmd(c); }
});
document.addEventListener('keyup', e => {
  held.delete(e.key);
  const c = keyMap[e.key];
  if (c && c !== 'S') sendCmd('S');
});

// ── Скорость ───────────────────────────────────────────────
const slider = document.getElementById('spd-slider');
let speedTimer = null;
slider.addEventListener('input', () => {
  const pct = Math.round((slider.value - 50) / 1950 * 100);
  document.getElementById('spd-val').textContent = pct + '%';
  // Дебаунс — не спамить пока тянешь слайдер
  clearTimeout(speedTimer);
  speedTimer = setTimeout(() => send('V' + slider.value), 80);
});

// ── Камера ─────────────────────────────────────────────────
let camOn = false;
function toggleCam() {
  camOn = !camOn;
  const btn = document.getElementById('cam-btn');
  const img = document.getElementById('cam-img');
  const ph  = document.getElementById('cam-ph');
  const ip  = document.getElementById('cam-ip').value.trim();

  btn.textContent = camOn ? 'Выключить' : 'Включить';
  btn.classList.toggle('on', camOn);

  if (camOn) {
    ph.style.display = 'none';
    img.src = 'http://' + ip + '/stream';
    img.classList.add('on');
  } else {
    img.src = '';
    img.classList.remove('on');
    ph.style.display = 'block';
    document.getElementById('live-badge').classList.remove('on');
  }
}
function camOk()   { document.getElementById('live-badge').classList.add('on'); }
function camFail() {
  document.getElementById('live-badge').classList.remove('on');
  if (camOn) {
    document.getElementById('cam-ph').textContent = 'Камера недоступна — проверь IP AtomS3R';
    document.getElementById('cam-ph').style.display = 'block';
    document.getElementById('cam-img').classList.remove('on');
  }
}

// ── Старт ──────────────────────────────────────────────────
const colorMap = {
  white:'#ffffff', black:'#222222', red:'#f75f4f',
  blue:'#4f8ef7', green:'#4fcc8e', other:'#aaaaaa', unknown:'#555555'
};
const colorName = {
  white:'Valge', black:'Must', red:'Punane',
  blue:'Sinine', green:'Roheline', other:'Muu', unknown:'—'
};

function updateSensorUI(d) {
  // Kaugus
  const distEl = document.getElementById('s-dist');
  distEl.textContent = d.dist > 0 ? d.dist.toFixed(1) : '—';
  // Hoiatus kui liiga lähedal
  distEl.style.color = (d.dist > 0 && d.dist < 15) ? 'var(--danger)' : 'var(--text)';

  // Värv
  const c = d.color || 'unknown';
  document.getElementById('s-dot').style.background = colorMap[c] || '#555';
  document.getElementById('s-colorname').textContent = colorName[c] || c;
}

connect();
</script>
</body>
</html>
)rawhtml";

// ══════════════════════════════════════════════════════════
// HTTP
// ══════════════════════════════════════════════════════════

void handleRoot() {
  httpServer.sendHeader("Cache-Control", "public, max-age=3600");
  httpServer.send_P(200, "text/html", INDEX_HTML);
}

// AtomS3R saadab kaugusandmed siia iga 200ms
void handleSensor() {
  if (!httpServer.hasArg("plain")) { httpServer.send(400, "text/plain", "no body"); return; }
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, httpServer.arg("plain"))) { httpServer.send(400, "text/plain", "bad json"); return; }
  if (doc.containsKey("dist")) sensorDist = doc["dist"].as<float>();
  Serial.printf("Sensor received: dist=%.1f\n", sensorDist);
  httpServer.send(200, "text/plain", "ok");
}

// ESP32-CAM saadab QR koodid siia
void handleQr() {
  if (!httpServer.hasArg("plain")) { httpServer.send(400, "text/plain", "no body"); return; }
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, httpServer.arg("plain"))) { httpServer.send(400, "text/plain", "bad json"); return; }
  String qr = doc["qr"].as<String>();
  Serial.printf("QR received: %s\n", qr.c_str());
  // Saada brauserile WebSocket kaudu
  StaticJsonDocument<256> out;
  out["type"] = "qr";
  out["qr"]   = qr;
  String msg; serializeJson(out, msg);
  wsServer.broadcastTXT(msg);
  httpServer.send(200, "text/plain", "ok");
}

// ══════════════════════════════════════════════════════════
// SETUP / LOOP
// ══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  // Датчики I2C
  initSensors();

  // Пины моторов
  pinMode(L_PWM1, OUTPUT); pinMode(L_DIR1, OUTPUT);
  pinMode(L_PWM2, OUTPUT); pinMode(L_DIR2, OUTPUT);
  pinMode(R_PWM1, OUTPUT); pinMode(R_DIR1, OUTPUT);
  pinMode(R_PWM2, OUTPUT); pinMode(R_DIR2, OUTPUT);

  // Стоп по умолчанию
  setMotors(STOPPED, STOPPED, 0, 0);

  // WiFi точка доступа
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());

  // HTTP marsruudid
  httpServer.on("/", handleRoot);
  httpServer.on("/sensor", HTTP_POST, handleSensor);
  httpServer.on("/qr",     HTTP_POST, handleQr);
  httpServer.begin();

  // WebSocket на порту 81
  wsServer.begin();
  wsServer.onEvent(onWsEvent);

  Serial.println("Ready! http://192.168.4.1");
  Serial.println("WS:   ws://192.168.4.1:81");
}

void loop() {
  httpServer.handleClient();
  wsServer.loop();
  // Вывод сырых данных TCS34725 в Serial (для отладки)
if (tcs_ok && (millis() - lastColorPrintMs >= COLOR_PRINT_INTERVAL_MS)) {
    lastColorPrintMs = millis();
    uint16_t r, g, b, c;
    tcs.getRawData(&r, &g, &b, &c);
    String colorName = readColor();
    Serial.printf("COLOR_RAW: R=%4d G=%4d B=%4d C=%4d -> %s\n", r, g, b, c, colorName.c_str());
}
  updateSensors();

  // ── Автостоп ─────────────────────────────────────────────
  if (!autoStopped && (leftDir != STOPPED || rightDir != STOPPED)) {
    if (millis() - lastCmdMs > WS_TIMEOUT_MS) {
      setMotors(STOPPED, STOPPED, 0, 0);
      autoStopped = true;
      Serial.println("Autostop triggered");
    }
  }
}
