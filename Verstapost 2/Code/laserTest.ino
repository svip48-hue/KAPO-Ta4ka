#include <Wire.h>
#include "Adafruit_VL53L0X.h"

#define SDA_PIN   12
#define SCL_PIN   16

Adafruit_VL53L0X vl53;

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  
  Serial.println("Otsin VL53L0X...");
  if (vl53.begin(0x29, false, &Wire)) {
    Serial.println("VL53L0X OK!");
  } else {
    Serial.println("VL53L0X ei leitud!");
  }
}

void loop() {
  VL53L0X_RangingMeasurementData_t m;
  vl53.rangingTest(&m, false);
  if (m.RangeStatus != 4) {
    Serial.printf("Kaugus: %d mm\n", m.RangeMilliMeter);
  } else {
    Serial.println("Väljaspool vahemikku");
  }
  delay(200);
}