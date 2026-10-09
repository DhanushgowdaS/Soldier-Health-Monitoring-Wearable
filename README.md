# Soldier Health Monitoring Wearable

ESP32-based student prototype for wearable health/environment monitoring with a live LittleFS dashboard and Telegram alerts.

## Features
- MAX30102: heart-rate sampling. SpO2 remains unavailable until a validated algorithm is integrated.
- DHT11 (current firmware setting): ambient temperature and relative humidity; it does not measure core/body temperature. Change `DHT_TYPE` to `DHT22` if using a DHT22 module.
- MPU6050: motion estimate and heuristic possible-fall detection.
- UART GPS: coordinates and Google Maps link.
- I2C LCD, buzzer, ESP32 Wi-Fi web server, LittleFS dashboard, WebSocket telemetry, Telegram alerts.

> Educational prototype only. Not medically validated and must not be used as the sole basis for real-world emergency decisions.

## Structure
- `firmware/SoldierMonitor.ino` — ESP32 firmware only (sensor reading, alerts, HTTP/WebSocket servers, LCD, buzzer)
- `firmware/secrets.h.example` — credentials template; copy locally to `firmware/secrets.h`
- `data/index.html` — dashboard structure and labels
- `data/style.css` — dashboard appearance and responsive layout
- `data/script.js` — WebSocket connection, `/api/data` fallback, and live UI updates

**The website is not embedded as HTML/CSS/JavaScript inside the `.ino` sketch.** The ESP32 serves the separate files from LittleFS. Upload the sketch through Arduino IDE, then upload the *contents* of `data/` to LittleFS so `index.html`, `style.css`, and `script.js` are at the filesystem root.
- `docs/wiring.md`, `docs/architecture.md` (includes a Mermaid system architecture diagram), `docs/testing.md`

## Arduino IDE libraries
Install SparkFun MAX3010x Pulse and Proximity Sensor Library, DHT sensor library by Adafruit, Adafruit Unified Sensor, Adafruit MPU6050, Adafruit BusIO, TinyGPSPlus, a compatible LiquidCrystal_I2C library, WebSockets by Markus Sattler, and ArduinoJson 7.

Select a classic ESP32 Dev Module. Copy `firmware/secrets.h.example` to `firmware/secrets.h` and enter Wi-Fi/Telegram credentials locally. The real file is ignored by Git. Upload the sketch and upload the repository's `data/` folder to the ESP32 LittleFS partition with a compatible uploader.

## Important limits
- DHT11 measures ambient air temperature, not human body temperature.
- MAX30102 pulse readings are motion-sensitive. SpO2 is explicitly unavailable in the starter firmware.
- Fall detection is a heuristic and can generate false positives/negatives.
- GPS generally needs outdoor reception; Telegram needs internet access.
- Prototype only, not a medical device.
