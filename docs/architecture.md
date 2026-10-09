# Architecture and Alert Behaviour

## Data flow
1. ESP32 reads MAX30102 optical pulse samples for a tentative heart-rate estimate.
2. DHT22 measures ambient temperature and relative humidity.
3. MPU6050 acceleration/gyroscope data feeds a simple possible-fall heuristic.
4. GPS UART provides coordinates only while the fix is recent.
5. LCD shows a compact heart-rate/temperature/alert summary.
6. LittleFS serves `index.html`, `style.css`, and `script.js`; WebSocket port 81 broadcasts telemetry. `/api/data` is a fallback.
7. Configured threshold changes or a possible fall activate the buzzer and attempt a Telegram alert.

## Prototype defaults
- HR > 130 BPM or < 45 BPM when reading is valid.
- DHT22 ambient temperature > 38 °C.
- Relative humidity > 85%.
- Heuristic fall: impact over 2.5 g followed by low acceleration/gyro motion.

These are demo values, not medical recommendations. Tune and test in controlled conditions. The DHT22 reading must not be called body temperature.

## Telegram setup
Copy `firmware/secrets.h.example` to `firmware/secrets.h`; enter Wi-Fi name/password and Telegram bot token/chat ID. The actual secrets file is ignored by Git. Telegram requires internet access and has a one-minute cooldown.

## Known limits
SpO2 is not implemented in this starter and displays unavailable. MAX30102 HR is motion/contact sensitive. Fall detection is a crude heuristic with false positives/negatives. GPS may not fix indoors. Not a medical device; don't rely on it for actual emergency response.
