/*
  Piiri tuvastamise test — TCS34725
  ===================================
  Mõõdab 30 korda värvanduri reaktsiooniaega
  kui pind muutub valgelt mustaks (piiriala).

  Kasutamine:
    1. Aseta andur statsionaarselt VALGE pinna kohale
    2. Ava Serial Monitor (115200)
    3. Vajuta nuppu Serial Monitoris (saada 'S')
    4. Tõmba must paber/riie KIIRESTI anduri alla
    5. Korda 30 korda
    6. Tulemused kuvatakse automaatselt

  Pühendused:
    TCS34725 SDA → GPIO8
    TCS34725 SCL → GPIO9
    TCS34725 VCC → 5V
    TCS34725 GND → GND
*/

#include <Wire.h>
#include "Adafruit_TCS34725.h"

#define SDA_PIN 8
#define SCL_PIN 9

Adafruit_TCS34725 tcs = Adafruit_TCS34725(
  TCS34725_INTEGRATIONTIME_24MS,  // 24ms — kiirem kui 50ms
  TCS34725_GAIN_4X
);

// ── Värvituvastus ──────────────────────────────────────────
String getColor() {
  uint16_t r, g, b, c;
  tcs.getRawData(&r, &g, &b, &c);
  if (c > 3000) return "white";
  if (c < 500)  return "black";
  return "other";
}

// ── Testi andmed ───────────────────────────────────────────
const int N = 30;
float measurements[N];
int   count = 0;
bool  testRunning = false;
bool  waitingForChange = false;
unsigned long waitStart = 0;
String baseColor = "";

#define TIMEOUT_MS 3000  // max ooteaeg ühele mõõtmisele

// ── Statistika ─────────────────────────────────────────────
void printStats() {
  if (count == 0) { Serial.println("Andmeid pole!"); return; }

  // Sorteeri bubble sort
  float sorted[N];
  for (int i = 0; i < count; i++) sorted[i] = measurements[i];
  for (int i = 0; i < count - 1; i++)
    for (int j = 0; j < count - i - 1; j++)
      if (sorted[j] > sorted[j+1]) {
        float tmp = sorted[j]; sorted[j] = sorted[j+1]; sorted[j+1] = tmp;
      }

  float minV = sorted[0];
  float maxV = sorted[count-1];
  float median = sorted[count/2];

  float sum = 0;
  for (int i = 0; i < count; i++) sum += measurements[i];
  float mean = sum / count;

  float sq_sum = 0;
  for (int i = 0; i < count; i++) sq_sum += (measurements[i] - mean) * (measurements[i] - mean);
  float stddev = sqrt(sq_sum / count);

  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println("║   PIIRI TUVASTAMISE TESTI TULEMUSED  ║");
  Serial.println("╚══════════════════════════════════════╝");
  Serial.printf("  Mõõtmiste arv:  %d\n", count);
  Serial.println("  ─────────────────────────────────────");
  Serial.printf("  Min:            %.1f ms\n", minV);
  Serial.printf("  Mediaan:        %.1f ms\n", median);
  Serial.printf("  Max:            %.1f ms\n", maxV);
  Serial.printf("  Std dev:        %.1f ms\n", stddev);
  Serial.println("  ─────────────────────────────────────");

  // Roboti kiirus ja pidurdusteekond
  float speed_ms = 0.10;  // m/s — muuda oma mõõdetud väärtusega
  float ws_latency = 4.0; // ms — WebSocket latentsus VP2-st
  float total_reaction = median + ws_latency;
  float braking_cm = speed_ms * (total_reaction / 1000.0) * 100.0;

  Serial.println("  ARVUTUSED:");
  Serial.printf("  Anduri reaktsioon (mediaan): %.1f ms\n", median);
  Serial.printf("  + WebSocket latentsus:       %.1f ms\n", ws_latency);
  Serial.printf("  = Kogu reaktsiooniaeg:       %.1f ms\n", total_reaction);
  Serial.printf("  Roboti kiirus:               %.2f m/s\n", speed_ms);
  Serial.printf("  Pidurdusteekond:             %.1f cm\n", braking_cm);
  Serial.println("  ─────────────────────────────────────");
  if (braking_cm < 10.0) {
    Serial.println("  ✅ LÄBITUD — pidurdusteekond < 10 cm");
  } else {
    Serial.println("  ⚠️  EBAÕNNESTUS — pidurdusteekond > 10 cm");
    Serial.println("     Lahendus: vähenda kiirust või anduri intervalli");
  }
  Serial.println("  ─────────────────────────────────────");

  // Kõik mõõtmised (CSV Jupyter Labi jaoks)
  Serial.println("\n  CSV (kopeeri Jupyter Labi):");
  Serial.print("  measurements = [");
  for (int i = 0; i < count; i++) {
    Serial.printf("%.1f", measurements[i]);
    if (i < count - 1) Serial.print(", ");
  }
  Serial.println("]");
  Serial.println("\nSaada 'S' uue testi alustamiseks");
}

// ── Setup ──────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!tcs.begin(0x29, &Wire)) {
    Serial.println("❌ TCS34725 ei leitud! Kontrolli ühendusi.");
    while (1) delay(100);
  }

  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println("║     PIIRI TUVASTAMISE TEST v1.0      ║");
  Serial.println("╚══════════════════════════════════════╝");
  Serial.println("  TCS34725 OK!");
  Serial.println("  ─────────────────────────────────────");
  Serial.println("  JUHEND:");
  Serial.println("  1. Aseta andur VALGE pinna kohale");
  Serial.println("  2. Saada 'S' testi alustamiseks");
  Serial.println("  3. Tõmba MUST paber KIIRESTI alla");
  Serial.println("  4. Korda 30 korda");
  Serial.println("  ─────────────────────────────────────");
}

// ── Loop ───────────────────────────────────────────────────
void loop() {
  // Käskluste vastuvõtt
  if (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'S' || cmd == 's') {
      count = 0;
      testRunning = true;
      Serial.println("\n🟢 TEST ALGAS!");
      Serial.println("  Aseta andur VALGE pinna kohale...");
      delay(1000);

      // Tuvasta baasvärvus
      baseColor = getColor();
      Serial.printf("  Baasvärvus: %s\n", baseColor.c_str());
      if (baseColor != "white") {
        Serial.println("  ⚠️  HOIATUS: Aluspind ei ole valge!");
        Serial.println("  Aseta andur valge pinna kohale ja vajuta 'S' uuesti.");
        testRunning = false;
        return;
      }
      Serial.printf("\n  Mõõtmine 1/%d — tõmba must paber alla!\n", N);
      waitingForChange = true;
      waitStart = millis();
    }
  }

  if (!testRunning || !waitingForChange) return;

  // Mõõda reaktsiooniaeg
  String current = getColor();
  unsigned long now = millis();

  if (current == "black") {
    // Muutus tuvastatud!
    float reaction_ms = now - waitStart;
    measurements[count] = reaction_ms;
    count++;

    Serial.printf("  ✅ %d/%d: %.0f ms\n", count, N, reaction_ms);

    if (count >= N) {
      // Kõik mõõtmised tehtud
      testRunning = false;
      waitingForChange = false;
      printStats();
      return;
    }

    // Järgmine mõõtmine — oota kuni valge tagasi
    Serial.println("  ↩️  Tõsta must paber ära (valge alla)...");
    delay(200);

    // Oota kuni valge tagasi (max 5 sek)
    unsigned long waitWhite = millis();
    while (getColor() != "white" && millis() - waitWhite < 5000) {
      delay(20);
    }
    delay(300);  // väike stabiilsuspaus

    Serial.printf("\n  Mõõtmine %d/%d — tõmba must paber alla!\n", count + 1, N);
    waitStart = millis();

  } else if (now - waitStart > TIMEOUT_MS) {
    // Timeout — ei tuvastanud muutust
    Serial.printf("  ⏱️  %d/%d: TIMEOUT (>%d ms) — proovi uuesti\n", count + 1, N, TIMEOUT_MS);
    Serial.printf("\n  Mõõtmine %d/%d — tõmba must paber alla!\n", count + 1, N);
    waitStart = millis();
  }
}
