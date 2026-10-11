#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"

MAX30105 sensor;
const byte N = 4;
float rates[N];
byte idx = 0, cnt = 0;
unsigned long lastBeat = 0, lastPrint = 0;

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 not found");
    while (1);
  }
  sensor.setup(0x1F, 4, 2, 400, 411, 4096);
}

void loop() {
  long ir = sensor.getIR();

  if (ir < 50000) {
    cnt = 0;
    idx = 0;
    lastBeat = 0;
    if (millis() - lastPrint > 500) {
      lastPrint = millis();
      Serial.println("No finger");
    }
    return;
  }

  if (checkForBeat(ir)) {
    unsigned long now = millis();
    if (lastBeat) {
      float b = 60000.0 / (now - lastBeat);
      if (b > 40 && b < 180) {
        rates[idx] = b;
        idx = (idx + 1) % N;
        if (cnt < N) cnt++;
      }
    }
    lastBeat = now;
  }

  if (millis() - lastPrint > 500) {
    lastPrint = millis();
    if (cnt < N) {
      Serial.printf("IR=%ld Measuring...\n", ir);
    } else {
      float sum = 0;
      for (byte i = 0; i < N; i++) sum += rates[i];
      Serial.printf("IR=%ld BPM=%.0f\n", ir, sum / N);
    }
  }
}