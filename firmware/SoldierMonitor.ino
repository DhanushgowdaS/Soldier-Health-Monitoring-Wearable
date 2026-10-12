/*
 * Soldier Health Monitoring Wearable
 * Main ESP32 firmware
 *
 * Sensors: MAX30102, DHT11 (select DHT22 in DHT_TYPE if using DHT22), MPU6050, UART GPS
 * Outputs: I2C LCD, buzzer, Telegram alerts
 * Web UI: separate files in /data, served from ESP32 LittleFS
 * Network: HTTP port 80, WebSocket port 81
 *
 * Educational prototype only. Not a medical device.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <TinyGPSPlus.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <MAX30105.h>
#include "heartRate.h"
#include "spo2_algorithm.h"
#include "secrets.h"

// ------------------------------ Pin assignments ------------------------------
constexpr int DHT_PIN = 4;
constexpr int DHT_TYPE = DHT11;
constexpr int GPS_RX_PIN = 16;
constexpr int GPS_TX_PIN = 17;
constexpr int BUZZER_PIN = 25;
constexpr int I2C_SDA_PIN = 21;
constexpr int I2C_SCL_PIN = 22;
constexpr uint8_t LCD_I2C_ADDRESS = 0x27;

// ------------------------------ Thresholds/timing -----------------------------
constexpr float AMBIENT_HIGH_C = 37.0f; // Ambient-temperature alert threshold
constexpr float HUMIDITY_HIGH_PERCENT = 85.0f;
constexpr int HEART_RATE_HIGH_BPM = 100; // Typical adult resting upper limit
constexpr int HEART_RATE_LOW_BPM = 60;  // Typical adult resting lower limit
constexpr float SPO2_LOW_PERCENT = 95.0f; // Typical healthy adult range is usually 95-100%
constexpr uint32_t SENSOR_INTERVAL_MS = 2000;
constexpr uint32_t LCD_INTERVAL_MS = 1000;
constexpr uint32_t WEBSOCKET_INTERVAL_MS = 1000;
constexpr uint32_t LCD_PAGE_INTERVAL_MS = 5000;

// ------------------------------ Hardware objects ------------------------------
WebServer server(80);
WebSocketsServer webSocket(81);
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
TinyGPSCustom gpsView(gps, "GPGSV", 3);
TinyGPSCustom beidouView(gps, "BDGSV", 3);
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, 16, 2);
Adafruit_MPU6050 mpu;
MAX30105 pulseSensor;

// ------------------------------ Shared state ----------------------------------
struct MonitorState {
    float heartRateBpm = NAN;
    float spo2Percent = NAN;
    float ambientTemperatureC = NAN;
    float humidityPercent = NAN;
    double latitude = 0.0;
    double longitude = 0.0;

    bool gpsFix = false;
    bool mpuAvailable = false;
    bool pulseSensorAvailable = false;
    bool possibleFall = false;
    bool buzzerOn = false;

    String activity = "Unknown";
    String status = "STARTING";
    String alertReason = "";
};

MonitorState monitor;
float recentHeartRates[4] = {0, 0, 0, 0};
uint32_t irBuffer[BUFFER_SIZE] = {0};
uint32_t redBuffer[BUFFER_SIZE] = {0};
int32_t spo2SampleCount = 0;
uint16_t samplesSinceSpo2Calculation = 0;
uint8_t heartRateIndex = 0;

uint32_t lastBeatMs = 0;
uint32_t lastSensorReadMs = 0;
uint32_t lastLcdUpdateMs = 0;
uint32_t lastWebSocketBroadcastMs = 0;
uint32_t impactDetectedMs = 0;
uint32_t lastMotionSampleMs = 0;
uint32_t fallStillStartMs = 0;
float gyroXOffsetRadS = 0.0f;
float gyroYOffsetRadS = 0.0f;
float gyroZOffsetRadS = 0.0f;
uint32_t lastLcdPageSwitchMs = 0;
uint32_t lastGpsDiagnosticMs = 0;
uint32_t lastPulseDiagnosticMs = 0;
long lastInfraredValue = 0;
uint32_t lastFingerSignalMs = 0;

bool fallImpactPending = false;
bool fallCandidateReported = false;
bool showIpOnLcd = false;
bool telegramConfigured = false;
String lastAlertReason = "";

// ================================= Helpers ====================================
bool isValidReading(float value) {
    return isfinite(value);
}

String getGoogleMapsUrl() {
    if (!monitor.gpsFix) {
        return "";
    }

    return "https://maps.google.com/?q=" +
           String(monitor.latitude, 6) + "," +
           String(monitor.longitude, 6);
}

// ================================ Dashboard ===================================
String buildTelemetryJson() {
    JsonDocument document;

    if (isValidReading(monitor.heartRateBpm)) {
        document["heartRate"] = monitor.heartRateBpm;
    } else {
        document["heartRate"] = nullptr;
    }

    if (isValidReading(monitor.ambientTemperatureC)) {
        document["ambientC"] = monitor.ambientTemperatureC;
    } else {
        document["ambientC"] = nullptr;
    }

    if (isValidReading(monitor.humidityPercent)) {
        document["humidity"] = monitor.humidityPercent;
    } else {
        document["humidity"] = nullptr;
    }

    if (isValidReading(monitor.spo2Percent)) {
        document["spo2"] = monitor.spo2Percent;
    } else {
        document["spo2"] = nullptr;
    }
    document["gpsFix"] = monitor.gpsFix;

    if (monitor.gpsFix) {
        document["latitude"] = monitor.latitude;
        document["longitude"] = monitor.longitude;
    } else {
        document["latitude"] = nullptr;
        document["longitude"] = nullptr;
    }

    document["mapsUrl"] = getGoogleMapsUrl();
    // Activity comes from the MPU6050 motion classifier, not the MAX30102.
    document["mpuAvailable"] = monitor.mpuAvailable;
    document["activity"] = monitor.mpuAvailable ? monitor.activity : "MPU6050 unavailable";
    document["fallDetected"] = monitor.possibleFall;
    document["fallChecking"] = fallImpactPending;
    document["fallState"] = !monitor.mpuAvailable
        ? "SENSOR UNAVAILABLE"
        : (monitor.possibleFall
            ? "POSSIBLE FALL"
            : (fallImpactPending ? "CHECKING EVENT" : "NORMAL"));
    document["buzzerOn"] = monitor.buzzerOn;
    document["status"] = monitor.status;
    document["alertReason"] = monitor.alertReason;
    document["uptimeSeconds"] = millis() / 1000;

    String json;
    serializeJson(document, json);
    return json;
}

void broadcastTelemetry() {
    // WebSocketsServer expects a mutable String reference in this library version.
    String telemetry = buildTelemetryJson();
    webSocket.broadcastTXT(telemetry);
}

void handleTelemetryRequest() {
    server.send(200, "application/json", buildTelemetryJson());
}

void serveLittleFsFile(const char* path, const char* contentType) {
    File file = LittleFS.open(path, "r");

    if (!file) {
        server.send(500, "text/plain", String("Missing LittleFS file: ") + path);
        return;
    }

    server.streamFile(file, contentType);
    file.close();
}

// ================================ Telegram ====================================
bool sendTelegramAlert(const String& reason) {
    if (!telegramConfigured || WiFi.status() != WL_CONNECTED) {
        Serial.println("Telegram skipped: Wi-Fi or credentials unavailable.");
        return false;
    }

    WiFiClientSecure tlsClient;
    // Prototype only: production firmware should validate Telegram's TLS certificate.
    tlsClient.setInsecure();

    HTTPClient http;
    const String endpoint =
        String("https://api.telegram.org/bot") +
        TELEGRAM_BOT_TOKEN + "/sendMessage";

    if (!http.begin(tlsClient, endpoint)) {
        Serial.println("Telegram error: could not start HTTPS request.");
        return false;
    }

    http.addHeader("Content-Type", "application/json");

    const bool normalStatus = reason == "NORMAL";
    String message;

    if (normalStatus) {
        message = "🟢 <b>SOLDIER STATUS: NO ACTIVE ALERTS</b>\n"
                  "🟢 All available readings are within configured limits.";
    } else {
        message = "🚨 <b>SOLDIER EMERGENCY ALERT</b>\n"
                  "🔴 <b>Fault:</b> " + reason;
    }

    message += "\n\n❤️ <b>Heart Rate:</b> ";
    message += isValidReading(monitor.heartRateBpm)
        ? String(monitor.heartRateBpm, 0) + " BPM"
        : String("Unavailable");

    message += "\n🫁 <b>SpO₂:</b> ";
    message += isValidReading(monitor.spo2Percent)
        ? String(monitor.spo2Percent, 0) + " %"
        : String("Unavailable (waiting for valid signal)");
    message += "\n🌡️ <b>Ambient Temperature:</b> ";
    message += isValidReading(monitor.ambientTemperatureC)
        ? String(monitor.ambientTemperatureC, 1) + " °C"
        : String("Unavailable");

    message += "\n💧 <b>Humidity:</b> ";
    message += isValidReading(monitor.humidityPercent)
        ? String(monitor.humidityPercent, 0) + " %"
        : String("Unavailable");

    message += "\n🏃 <b>Activity:</b> " + monitor.activity;
    message += "\n🛡️ <b>Fall:</b> ";
    message += monitor.possibleFall ? "🔴 POSSIBLE FALL" : "🟢 Not detected";

    if (monitor.gpsFix) {
        message += "\n\n📍 <b>Location:</b> ";
        message += String(monitor.latitude, 6) + ", ";
        message += String(monitor.longitude, 6);
        message += "\n🗺️ <a href=\"";
        message += getGoogleMapsUrl();
        message += "\">Open location in Google Maps</a>";
    } else {
        message += "\n\n📍 <b>Location:</b> GPS fix unavailable";
    }

    JsonDocument request;
    request["chat_id"] = TELEGRAM_CHAT_ID;
    request["text"] = message;
    request["parse_mode"] = "HTML";
    request["disable_web_page_preview"] = true;

    String requestBody;
    serializeJson(request, requestBody);

    const int httpStatus = http.POST(requestBody);
    Serial.printf("Telegram HTTP status: %d\n", httpStatus);
    http.end();

    if (httpStatus >= 200 && httpStatus < 300) {
        Serial.println("Telegram message sent.");
        return true;
    }
    Serial.printf("Telegram send failed; HTTP status: %d\n", httpStatus);
    return false;
}

// ================================= Sensors ====================================
void readHeartRate() {
    if (!monitor.pulseSensorAvailable) return;

    // Drain available FIFO samples so both red and IR channels are captured.
    pulseSensor.check();
    while (pulseSensor.available()) {
        const uint32_t infraredValue = pulseSensor.getIR();
        const uint32_t redValue = pulseSensor.getRed();
        pulseSensor.nextSample();
        lastInfraredValue = static_cast<long>(infraredValue);

        if (infraredValue >= 50000) {
            lastFingerSignalMs = millis();
        }

        if (infraredValue < 50000) {
            monitor.heartRateBpm = NAN;
            monitor.spo2Percent = NAN;
            spo2SampleCount = 0;
            samplesSinceSpo2Calculation = 0;
            lastBeatMs = 0;
            continue;
        }

        // Keep the existing IR-based heart-rate calculation.
        if (checkForBeat(static_cast<long>(infraredValue))) {
            const uint32_t now = millis();
            if (lastBeatMs != 0) {
                const float bpm = 60000.0f / (now - lastBeatMs);
                if (bpm >= 35 && bpm <= 220) recentHeartRates[heartRateIndex++ % 4] = bpm;
                float sum = 0;
                int count = 0;
                for (float reading : recentHeartRates) {
                    if (reading > 0) { sum += reading; count++; }
                }
                if (count > 0) monitor.heartRateBpm = sum / count;
            }
            lastBeatMs = now;
        }

        if (spo2SampleCount < BUFFER_SIZE) {
            irBuffer[spo2SampleCount] = infraredValue;
            redBuffer[spo2SampleCount] = redValue;
            spo2SampleCount++;
        } else {
            memmove(irBuffer, irBuffer + 1, (BUFFER_SIZE - 1) * sizeof(irBuffer[0]));
            memmove(redBuffer, redBuffer + 1, (BUFFER_SIZE - 1) * sizeof(redBuffer[0]));
            irBuffer[BUFFER_SIZE - 1] = infraredValue;
            redBuffer[BUFFER_SIZE - 1] = redValue;
        }

        if (spo2SampleCount >= BUFFER_SIZE && ++samplesSinceSpo2Calculation >= 25) {
            samplesSinceSpo2Calculation = 0;
            int32_t calculatedSpO2 = -1, calculatedHeartRate = -1;
            int8_t spo2Valid = 0, heartRateValid = 0;
            maxim_heart_rate_and_oxygen_saturation(
                irBuffer, BUFFER_SIZE, redBuffer,
                &calculatedSpO2, &spo2Valid,
                &calculatedHeartRate, &heartRateValid);

            if (spo2Valid && calculatedSpO2 >= 70 && calculatedSpO2 <= 100) {
                monitor.spo2Percent = static_cast<float>(calculatedSpO2);
            } else {
                monitor.spo2Percent = NAN;
            }
        }
    }

    // Clear stale pulse readings when the finger is removed or no fresh
    // optical samples have arrived. Unavailable is not treated as a safe BPM/SpO2.
    if (millis() - lastFingerSignalMs >= 2000) {
        lastInfraredValue = 0;
        monitor.heartRateBpm = NAN;
        monitor.spo2Percent = NAN;
        spo2SampleCount = 0;
        samplesSinceSpo2Calculation = 0;
        lastBeatMs = 0;
        for (float &reading : recentHeartRates) reading = 0;
    }
}

void readGps() {
    while (gpsSerial.available() > 0) {
        const char c = static_cast<char>(gpsSerial.read());
        gps.encode(c);
    }

    monitor.gpsFix = gps.location.isValid() && gps.location.age() < 5000;

    if (monitor.gpsFix) {
        monitor.latitude = gps.location.lat();
        monitor.longitude = gps.location.lng();
    }
}

void printGpsDiagnostic() {
    if (millis() - lastGpsDiagnosticMs < 2000) {
        return;
    }
    lastGpsDiagnosticMs = millis();

    static uint32_t lastCharsReported = 0;
    const uint32_t charsReceived = gps.charsProcessed();
    const uint32_t charsThisInterval = charsReceived - lastCharsReported;
    lastCharsReported = charsReceived;

    Serial.printf("GPS check | UART chars total: %lu | chars/2s: %lu | checksum errors: %lu | valid sentences: %lu | Used satellites: %u | ",
                  static_cast<unsigned long>(charsReceived),
                  static_cast<unsigned long>(charsThisInterval),
                  static_cast<unsigned long>(gps.failedChecksum()),
                  static_cast<unsigned long>(gps.passedChecksum()),
                  gps.satellites.isValid() ? gps.satellites.value() : 0);

    if (monitor.gpsFix) {
        Serial.printf("FIX | Lat=%.6f | Lng=%.6f | Location age=%lu ms\n",
                      monitor.latitude,
                      monitor.longitude,
                      static_cast<unsigned long>(gps.location.age()));
    } else {
        Serial.printf("NO FIX | Location valid=%s | Satellites valid=%s\n",
                      gps.location.isValid() ? "yes" : "no",
                      gps.location.isValid() ? "yes" : "no");
    }
}

// This board reports WHO_AM_I=0x70 (MPU-6500 family), so use
// compatible raw I2C registers instead of Adafruit_MPU6050's 0x68 ID check.
bool imuWriteRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(0x68);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

bool imuReadRegisters(uint8_t reg, uint8_t *buffer, uint8_t count) {
    Wire.beginTransmission(0x68);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)0x68, count) != count) return false;
    for (uint8_t i = 0; i < count; i++) buffer[i] = Wire.read();
    return true;
}

bool imuReadMotion(float &ax, float &ay, float &az,
                   float &gx, float &gy, float &gz) {
    uint8_t b[14];
    if (!imuReadRegisters(0x3B, b, sizeof(b))) return false;
    int16_t rawAx = (int16_t)((b[0] << 8) | b[1]);
    int16_t rawAy = (int16_t)((b[2] << 8) | b[3]);
    int16_t rawAz = (int16_t)((b[4] << 8) | b[5]);
    int16_t rawGx = (int16_t)((b[8] << 8) | b[9]);
    int16_t rawGy = (int16_t)((b[10] << 8) | b[11]);
    int16_t rawGz = (int16_t)((b[12] << 8) | b[13]);

    // Configured for +/-8 g and +/-500 degrees/second.
    ax = (rawAx / 4096.0f) * 9.80665f;
    ay = (rawAy / 4096.0f) * 9.80665f;
    az = (rawAz / 4096.0f) * 9.80665f;
    constexpr float DEGREES_TO_RADIANS = 0.01745329252f;
    gx = (rawGx / 65.5f) * DEGREES_TO_RADIANS;
    gy = (rawGy / 65.5f) * DEGREES_TO_RADIANS;
    gz = (rawGz / 65.5f) * DEGREES_TO_RADIANS;
    return true;
}

// Calibrate gyro bias while the sensor is completely still at startup.
void calibrateMotionGyro() {
    if (!monitor.mpuAvailable) return;

    Serial.println("Keep MPU sensor still: calibrating gyro...");
    const int samples = 300;
    double sumX = 0.0, sumY = 0.0, sumZ = 0.0;
    int validSamples = 0;

    for (int i = 0; i < samples; i++) {
        float ax, ay, az, gx, gy, gz;
        if (imuReadMotion(ax, ay, az, gx, gy, gz)) {
            sumX += gx; sumY += gy; sumZ += gz;
            validSamples++;
        }
        delay(5);
    }

    if (validSamples < samples * 0.8) {
        monitor.mpuAvailable = false;
        Serial.println("IMU read failed during calibration.");
        return;
    }

    gyroXOffsetRadS = sumX / validSamples;
    gyroYOffsetRadS = sumY / validSamples;
    gyroZOffsetRadS = sumZ / validSamples;
    Serial.println("IMU gyro calibration complete.");
}

// Sample every 20 ms. Ordinary axis changes indicate movement only.
// A possible fall requires an impact/freefall-like event followed by stillness.
void readMotionSensor() {
    if (!monitor.mpuAvailable) return;

    const uint32_t now = millis();
    if (now - lastMotionSampleMs < 20) return;
    lastMotionSampleMs = now;

    float accelerationX, accelerationY, accelerationZ;
    float rawGyroX, rawGyroY, rawGyroZ;
    if (!imuReadMotion(accelerationX, accelerationY, accelerationZ,
                       rawGyroX, rawGyroY, rawGyroZ)) {
        Serial.println("IMU register read failed.");
        monitor.mpuAvailable = false;
        monitor.activity = "MPU sensor read failed";
        return;
    }

    const float ax = accelerationX / 9.80665f;
    const float ay = accelerationY / 9.80665f;
    const float az = accelerationZ / 9.80665f;
    const float accelerationMagnitude = sqrtf(ax * ax + ay * ay + az * az);

    const float gx = rawGyroX - gyroXOffsetRadS;
    const float gy = rawGyroY - gyroYOffsetRadS;
    const float gz = rawGyroZ - gyroZOffsetRadS;
    const float gyroMagnitudeDps =
        sqrtf(gx * gx + gy * gy + gz * gz) * 57.29578f;

    // Ignore startup sensor settling and require a stronger impact.
    // Freefall-like readings alone are too prone to false positives on this module.
    constexpr float IMPACT_THRESHOLD_G = 3.0f;
    constexpr float STILL_ACCEL_TOLERANCE_G = 0.25f;
    constexpr float STILL_GYRO_THRESHOLD_DPS = 20.0f;
    constexpr uint32_t STARTUP_GUARD_MS = 8000;
    constexpr uint32_t STILL_TIME_MS = 2000;
    constexpr uint32_t EVENT_TIMEOUT_MS = 5000;
    constexpr uint32_t FALL_ALERT_HOLD_MS = 10000;
    static const uint32_t motionMonitoringStartedMs = millis();
    static uint32_t fallAlertStartedMs = 0;

    const bool suddenEvent =
        (now - motionMonitoringStartedMs >= STARTUP_GUARD_MS) &&
        accelerationMagnitude >= IMPACT_THRESHOLD_G;

    const bool appearsStill =
        fabsf(accelerationMagnitude - 1.0f) <= STILL_ACCEL_TOLERANCE_G &&
        gyroMagnitudeDps <= STILL_GYRO_THRESHOLD_DPS;

    if (suddenEvent && !fallImpactPending && !monitor.possibleFall) {
        fallImpactPending = true;
        fallCandidateReported = false;
        impactDetectedMs = now;
        fallStillStartMs = 0;
        Serial.println("IMU: sudden impact; checking for possible fall.");
    }

    if (fallImpactPending) {
        if (appearsStill) {
            if (fallStillStartMs == 0) fallStillStartMs = now;

            if (now - fallStillStartMs >= STILL_TIME_MS) {
                monitor.possibleFall = true;
                fallAlertStartedMs = now;
                fallCandidateReported = true;
                fallImpactPending = false;
                Serial.println("IMU: POSSIBLE FALL alert started.");
            }
        } else {
            fallStillStartMs = 0;
        }

        if (fallImpactPending && now - impactDetectedMs >= EVENT_TIMEOUT_MS) {
            fallImpactPending = false;
            fallStillStartMs = 0;
            Serial.println("IMU: event expired; no fall alert.");
        }
    }

    // Automatically clear the heuristic alert after 10 seconds.
    // This is a demo reset, not proof that a wearer is safe.
    if (monitor.possibleFall && fallAlertStartedMs != 0 &&
        now - fallAlertStartedMs >= FALL_ALERT_HOLD_MS) {
        monitor.possibleFall = false;
        fallAlertStartedMs = 0;
        Serial.println("IMU: possible-fall alert cleared; monitoring resumed.");
    }

    const bool moving =
        fabsf(accelerationMagnitude - 1.0f) > STILL_ACCEL_TOLERANCE_G ||
        gyroMagnitudeDps > STILL_GYRO_THRESHOLD_DPS;

    if (monitor.possibleFall) {
        monitor.activity = "Possible fall";
    } else if (fallImpactPending) {
        monitor.activity = "Checking event";
    } else {
        monitor.activity = moving ? "Movement detected" : "Stationary";
    }
}

// =============================== Alert logic ==================================
void evaluateAlerts() {
    String reason;

    if (isValidReading(monitor.heartRateBpm) &&
        monitor.heartRateBpm > HEART_RATE_HIGH_BPM) {
        reason += "High resting heart rate; ";
    }

    if (isValidReading(monitor.heartRateBpm) &&
        monitor.heartRateBpm < HEART_RATE_LOW_BPM) {
        reason += "Low resting heart rate; ";
    }

    if (isValidReading(monitor.spo2Percent) &&
        monitor.spo2Percent < SPO2_LOW_PERCENT) {
        reason += "Low SpO2; ";
    }

    if (isValidReading(monitor.ambientTemperatureC) &&
        monitor.ambientTemperatureC > AMBIENT_HIGH_C) {
        reason += "High ambient temperature; ";
    }

    if (isValidReading(monitor.humidityPercent) &&
        monitor.humidityPercent > HUMIDITY_HIGH_PERCENT) {
        reason += "High humidity; ";
    }

    if (monitor.possibleFall) {
        reason += "Possible fall detected; ";
    }

    monitor.alertReason = reason;
    monitor.buzzerOn = reason.length() > 0;
    digitalWrite(BUZZER_PIN, monitor.buzzerOn ? HIGH : LOW);

    monitor.status = monitor.buzzerOn
        ? "WARNING"
        : (monitor.gpsFix ? "MONITORING" : "WAITING FOR GPS");

    // Send immediately on each change in the active critical-condition set.
    // There is intentionally no global 60-second cooldown: a new danger must
    // not be delayed by an earlier Telegram message. Failed sends are retried
    // on the next evaluation cycle, while a successfully sent state is not spammed.
    if (!reason.isEmpty() && reason != lastAlertReason) {
        if (sendTelegramAlert(reason)) {
            lastAlertReason = reason;
        }
    } else if (reason.isEmpty() && !lastAlertReason.isEmpty()) {
        // Pulse readings may be unavailable when no finger is on the sensor;
        // environmental and motion alerts still run independently. The NORMAL
        // message reports unavailable pulse values rather than inventing values.
        if (sendTelegramAlert("NORMAL")) {
            lastAlertReason = "";
        }
    }
}

// =================================== LCD ======================================
void updateLcd() {
    if (millis() - lastLcdPageSwitchMs >= LCD_PAGE_INTERVAL_MS) {
        showIpOnLcd = !showIpOnLcd;
        lastLcdPageSwitchMs = millis();
    }

    lcd.clear();

    if (showIpOnLcd) {
        lcd.setCursor(0, 0);

        if (WiFi.status() == WL_CONNECTED) {
            lcd.print("Dashboard IP:");
            lcd.setCursor(0, 1);
            lcd.print(WiFi.localIP().toString());
        } else {
            lcd.print("WiFi not connected");
            lcd.setCursor(0, 1);
            lcd.print("Check credentials");
        }
        return;
    }

    lcd.setCursor(0, 0);
    lcd.print("HR:");
    lcd.print(isValidReading(monitor.heartRateBpm)
        ? String((int)monitor.heartRateBpm)
        : String("--"));

    lcd.print(" T:");
    lcd.print(isValidReading(monitor.ambientTemperatureC)
        ? String(monitor.ambientTemperatureC, 0)
        : String("--"));

    lcd.setCursor(0, 1);
    if (monitor.possibleFall) {
        lcd.print("FALL ALERT");
    } else if (monitor.buzzerOn) {
        lcd.print("HEALTH WARNING");
    } else if (monitor.gpsFix) {
        lcd.print("GPS OK");
    } else {
        lcd.print("GPS SEARCH");
    }

    if (isValidReading(monitor.humidityPercent)) {
        lcd.print(" ");
        lcd.print((int)monitor.humidityPercent);
        lcd.print("%");
    }
}

// ============================== Network setup ================================
void connectToWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("Connecting to Wi-Fi");
    const uint32_t startMs = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - startMs < 15000) {
        delay(250);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        const String ipAddress = WiFi.localIP().toString();

        Serial.println();
        Serial.println("Wi-Fi connected.");
        Serial.print("ESP32 IP: ");
        Serial.println(ipAddress);
        Serial.print("Dashboard URL: http://");
        Serial.println(ipAddress);

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("WiFi Connected");
        lcd.setCursor(0, 1);
        lcd.print(ipAddress);
    } else {
        Serial.println();
        Serial.println("Wi-Fi connection failed.");
        Serial.println("Check firmware/secrets.h.");

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("WiFi not connected");
        lcd.setCursor(0, 1);
        lcd.print("Check credentials");
    }
}

void configureWebServer() {
    server.on("/", HTTP_GET, []() {
        serveLittleFsFile("/index.html", "text/html");
    });

    server.on("/style.css", HTTP_GET, []() {
        serveLittleFsFile("/style.css", "text/css");
    });

    server.on("/script.js", HTTP_GET, []() {
        serveLittleFsFile("/script.js", "application/javascript");
    });

    server.on("/api/data", HTTP_GET, handleTelemetryRequest);
    server.begin();

    webSocket.begin();
    webSocket.onEvent([](uint8_t, WStype_t type, uint8_t*, size_t) {
        if (type == WStype_CONNECTED) {
            broadcastTelemetry();
        }
    });

    Serial.println("HTTP server started on port 80.");
    Serial.println("WebSocket server started on port 81.");
}

// ============================ Arduino entry points ============================
void setup() {
    Serial.begin(115200);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    lcd.init();
    lcd.backlight();
    lcd.print("Soldier Monitor");

    dht.begin();

    // WHO_AM_I is 0x70 on this compatible MPU-6500-family sensor.
    // Check the ID, wake it, and configure raw-register ranges directly.
    uint8_t imuId = 0;
    monitor.mpuAvailable = imuReadRegisters(0x75, &imuId, 1) &&
                           (imuId == 0x70 || imuId == 0x68);
    if (monitor.mpuAvailable) {
        const bool configured =
            imuWriteRegister(0x6B, 0x01) &&  // Wake, PLL clock
            imuWriteRegister(0x1C, 0x10) &&  // Accelerometer +/-8 g
            imuWriteRegister(0x1B, 0x08) &&  // Gyroscope +/-500 dps
            imuWriteRegister(0x1A, 0x04);    // DLPF bandwidth
        delay(100);
        monitor.mpuAvailable = configured;
    }
    if (monitor.mpuAvailable) {
        Serial.printf("Motion sensor detected; WHO_AM_I=0x%02X.\n", imuId);
        calibrateMotionGyro();
    } else {
        Serial.printf("Motion sensor unavailable; WHO_AM_I=0x%02X.\n", imuId);
    }

    monitor.pulseSensorAvailable = pulseSensor.begin(Wire, I2C_SPEED_FAST);
    if (monitor.pulseSensorAvailable) {
        // Match the settings used by the working standalone MAX30102 test.
        pulseSensor.setup(0x1F, 4, 2, 400, 411, 4096);
        pulseSensor.setPulseAmplitudeRed(0x0A);
        pulseSensor.setPulseAmplitudeGreen(0);
        Serial.println("MAX30102 detected.");
    } else {
        Serial.println("MAX30102 not detected.");
    }

    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed.");
    } else {
        Serial.println("LittleFS mounted.");
    }

    connectToWiFi();

    telegramConfigured =
        String(TELEGRAM_BOT_TOKEN).indexOf("REPLACE") < 0 &&
        String(TELEGRAM_CHAT_ID).indexOf("REPLACE") < 0;

    Serial.println(telegramConfigured
        ? "Telegram credentials appear configured."
        : "Telegram credentials missing; alerts disabled.");

    configureWebServer();
}

void loop() {
    server.handleClient();
    webSocket.loop();
    readGps();
    printGpsDiagnostic();

    // Sample MAX30102 continuously: beat detection can miss pulses if called
    // only once every SENSOR_INTERVAL_MS (2 seconds).
    readHeartRate();

    if (millis() - lastPulseDiagnosticMs >= 1000) {
        lastPulseDiagnosticMs = millis();
        if (!monitor.pulseSensorAvailable) {
            Serial.println("MAX30102 status: NOT DETECTED");
        } else if (lastInfraredValue < 50000) {
            Serial.printf("MAX30102 status: detected, no finger / low IR (IR=%ld)\n",
                          lastInfraredValue);
        } else if (isValidReading(monitor.heartRateBpm)) {
            Serial.printf("MAX30102 status: IR=%ld, Heart rate=%.0f BPM\n",
                          lastInfraredValue, monitor.heartRateBpm);
        } else {
            Serial.printf("MAX30102 status: measuring beat, IR=%ld\n",
                          lastInfraredValue);
        }
    }

    const uint32_t now = millis();

    // Sample the MPU6050 frequently so brief sudden events are less likely
    // to be missed; environmental sensors retain their slower interval.
    readMotionSensor();

    if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
        lastSensorReadMs = now;

        monitor.ambientTemperatureC = dht.readTemperature();
        monitor.humidityPercent = dht.readHumidity();

        evaluateAlerts();
    }

    if (now - lastLcdUpdateMs >= LCD_INTERVAL_MS) {
        lastLcdUpdateMs = now;
        updateLcd();
    }

    if (now - lastWebSocketBroadcastMs >= WEBSOCKET_INTERVAL_MS) {
        lastWebSocketBroadcastMs = now;
        broadcastTelemetry();
    }
}
