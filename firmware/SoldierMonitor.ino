/*
 * Soldier Health Monitoring Wearable
 * Main ESP32 firmware
 *
 * Sensors: MAX30102, DHT22, MPU6050, UART GPS
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
#include "secrets.h"

// ------------------------------ Pin assignments ------------------------------
constexpr int DHT_PIN = 4;
constexpr int DHT_TYPE = DHT22;
constexpr int GPS_RX_PIN = 16;
constexpr int GPS_TX_PIN = 17;
constexpr int BUZZER_PIN = 25;
constexpr int I2C_SDA_PIN = 21;
constexpr int I2C_SCL_PIN = 22;
constexpr uint8_t LCD_I2C_ADDRESS = 0x27;

// ------------------------------ Thresholds/timing -----------------------------
constexpr float AMBIENT_HIGH_C = 29.0f; // Temporary DHT22 test threshold
constexpr float HUMIDITY_HIGH_PERCENT = 85.0f;
constexpr int HEART_RATE_HIGH_BPM = 130;
constexpr int HEART_RATE_LOW_BPM = 45;
constexpr uint32_t SENSOR_INTERVAL_MS = 2000;
constexpr uint32_t LCD_INTERVAL_MS = 1000;
constexpr uint32_t WEBSOCKET_INTERVAL_MS = 1000;
constexpr uint32_t TELEGRAM_COOLDOWN_MS = 60000;
constexpr uint32_t LCD_PAGE_INTERVAL_MS = 5000;

// ------------------------------ Hardware objects ------------------------------
WebServer server(80);
WebSocketsServer webSocket(81);
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
DHT dht(DHT_PIN, DHT_TYPE);
LiquidCrystal_I2C lcd(LCD_I2C_ADDRESS, 16, 2);
Adafruit_MPU6050 mpu;
MAX30105 pulseSensor;

// ------------------------------ Shared state ----------------------------------
struct MonitorState {
    float heartRateBpm = NAN;
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
uint8_t heartRateIndex = 0;

uint32_t lastBeatMs = 0;
uint32_t lastSensorReadMs = 0;
uint32_t lastLcdUpdateMs = 0;
uint32_t lastWebSocketBroadcastMs = 0;
uint32_t lastTelegramSuccessMs = 0;
uint32_t impactDetectedMs = 0;
uint32_t lastLcdPageSwitchMs = 0;

bool fallImpactPending = false;
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

    // SpO2 remains unavailable until a suitable algorithm is integrated
    // and the readings have been validated against a reference device.
    document["spo2"] = nullptr;
    document["gpsFix"] = monitor.gpsFix;

    if (monitor.gpsFix) {
        document["latitude"] = monitor.latitude;
        document["longitude"] = monitor.longitude;
    } else {
        document["latitude"] = nullptr;
        document["longitude"] = nullptr;
    }

    document["mapsUrl"] = getGoogleMapsUrl();
    document["activity"] = monitor.activity;
    document["fallDetected"] = monitor.possibleFall;
    document["buzzerOn"] = monitor.buzzerOn;
    document["status"] = monitor.status;
    document["alertReason"] = monitor.alertReason;
    document["uptimeSeconds"] = millis() / 1000;

    String json;
    serializeJson(document, json);
    return json;
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
void sendTelegramAlert(const String& reason) {
    if (!telegramConfigured || WiFi.status() != WL_CONNECTED) {
        Serial.println("Telegram skipped: Wi-Fi or credentials unavailable.");
        return;
    }

    if (millis() - lastTelegramSuccessMs < TELEGRAM_COOLDOWN_MS) {
        Serial.println("Telegram skipped: cooldown active.");
        return;
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
        return;
    }

    http.addHeader("Content-Type", "application/json");

    const bool normalStatus = reason == "NORMAL";
    String message;

    if (normalStatus) {
        message = "🟢 <b>SOLDIER STATUS: NORMAL</b>\n"
                  "🟢 All monitored alert conditions are clear.";
    } else {
        message = "🚨 <b>SOLDIER EMERGENCY ALERT</b>\n"
                  "🔴 <b>Fault:</b> " + reason;
    }

    message += "\n\n❤️ <b>Heart Rate:</b> ";
    message += isValidReading(monitor.heartRateBpm)
        ? String(monitor.heartRateBpm, 0) + " BPM"
        : String("Unavailable");

    message += "\n🫁 <b>SpO₂:</b> Unavailable (algorithm not implemented)";
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
        lastTelegramSuccessMs = millis();
        Serial.println("Telegram message sent.");
    }
}

// ================================= Sensors ====================================
void readHeartRate() {
    if (!monitor.pulseSensorAvailable) {
        return;
    }

    const long infraredValue = pulseSensor.getIR();
    if (infraredValue < 50000) {
        monitor.heartRateBpm = NAN;
        return;
    }

    if (!checkForBeat(infraredValue)) {
        return;
    }

    const uint32_t now = millis();
    if (lastBeatMs != 0) {
        const float bpm = 60000.0f / (now - lastBeatMs);

        if (bpm >= 35 && bpm <= 220) {
            recentHeartRates[heartRateIndex++ % 4] = bpm;
        }

        float sum = 0;
        int count = 0;
        for (float reading : recentHeartRates) {
            if (reading > 0) {
                sum += reading;
                count++;
            }
        }

        if (count > 0) {
            monitor.heartRateBpm = sum / count;
        }
    }

    lastBeatMs = now;
}

void readGps() {
    while (gpsSerial.available()) {
        gps.encode(gpsSerial.read());
    }

    monitor.gpsFix = gps.location.isValid() && gps.location.age() < 5000;

    if (monitor.gpsFix) {
        monitor.latitude = gps.location.lat();
        monitor.longitude = gps.location.lng();
    }
}

void readMotionSensor() {
    if (!monitor.mpuAvailable) {
        return;
    }

    sensors_event_t acceleration;
    sensors_event_t gyroscope;
    sensors_event_t temperature;
    mpu.getEvent(&acceleration, &gyroscope, &temperature);

    const float ax = acceleration.acceleration.x / 9.80665f;
    const float ay = acceleration.acceleration.y / 9.80665f;
    const float az = acceleration.acceleration.z / 9.80665f;
    const float accelerationMagnitude = sqrtf(ax * ax + ay * ay + az * az);

    const float gx = gyroscope.gyro.x;
    const float gy = gyroscope.gyro.y;
    const float gz = gyroscope.gyro.z;
    const float gyroMagnitude = sqrtf(gx * gx + gy * gy + gz * gz);

    if (accelerationMagnitude > 2.5f) {
        fallImpactPending = true;
        impactDetectedMs = millis();
    }

    // Demonstration heuristic only; not validated fall detection.
    if (fallImpactPending &&
        millis() - impactDetectedMs > 1500 &&
        accelerationMagnitude < 0.45f &&
        gyroMagnitude < 0.35f) {
        monitor.possibleFall = true;
        monitor.activity = "Possible fall";
        fallImpactPending = false;
    }

    if (fallImpactPending && millis() - impactDetectedMs > 10000) {
        fallImpactPending = false;
    }

    if (!monitor.possibleFall) {
        monitor.activity =
            (accelerationMagnitude > 1.2f || gyroMagnitude > 0.6f)
                ? "Active"
                : "Stationary";
    }
}

// =============================== Alert logic ==================================
void evaluateAlerts() {
    String reason;

    if (isValidReading(monitor.heartRateBpm) &&
        monitor.heartRateBpm > HEART_RATE_HIGH_BPM) {
        reason += "High heart rate; ";
    }

    if (isValidReading(monitor.heartRateBpm) &&
        monitor.heartRateBpm < HEART_RATE_LOW_BPM) {
        reason += "Low heart rate; ";
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
    monitor.buzzerOn = !reason.isEmpty();
    digitalWrite(BUZZER_PIN, monitor.buzzerOn ? HIGH : LOW);

    monitor.status = monitor.buzzerOn
        ? "WARNING"
        : (monitor.gpsFix ? "MONITORING" : "WAITING FOR GPS");

    if (!reason.isEmpty() && reason != lastAlertReason) {
        sendTelegramAlert(reason);
        lastAlertReason = reason;
    } else if (reason.isEmpty() && !lastAlertReason.isEmpty()) {
        sendTelegramAlert("NORMAL");
        lastAlertReason = "";
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
            webSocket.broadcastTXT(buildTelemetryJson());
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

    monitor.mpuAvailable = mpu.begin();
    if (monitor.mpuAvailable) {
        mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
        mpu.setGyroRange(MPU6050_RANGE_500_DEG);
        mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
        Serial.println("MPU6050 detected.");
    } else {
        Serial.println("MPU6050 not detected.");
    }

    monitor.pulseSensorAvailable = pulseSensor.begin(Wire, I2C_SPEED_FAST);
    if (monitor.pulseSensorAvailable) {
        pulseSensor.setup();
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

    const uint32_t now = millis();

    if (now - lastSensorReadMs >= SENSOR_INTERVAL_MS) {
        lastSensorReadMs = now;

        monitor.ambientTemperatureC = dht.readTemperature();
        monitor.humidityPercent = dht.readHumidity();

        readHeartRate();
        readMotionSensor();
        evaluateAlerts();
    }

    if (now - lastLcdUpdateMs >= LCD_INTERVAL_MS) {
        lastLcdUpdateMs = now;
        updateLcd();
    }

    if (now - lastWebSocketBroadcastMs >= WEBSOCKET_INTERVAL_MS) {
        lastWebSocketBroadcastMs = now;
        webSocket.broadcastTXT(buildTelemetryJson());
    }
}
