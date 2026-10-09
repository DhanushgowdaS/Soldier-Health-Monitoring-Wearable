# Bring-up Checklist

## Power and bus
- [ ] Confirm module voltage requirements and common ground.
- [ ] Scan I2C addresses; verify LCD, MAX30102 and MPU6050.
- [ ] Check LCD and ensure ESP32 remains stable when Wi-Fi connects.

## Sensors
- [ ] DHT22 readings update; disconnected/invalid sensor is not mistaken for a normal reading.
- [ ] MAX30102 gives a stable tentative HR with still finger contact.
- [ ] MPU6050 activity updates; test fall heuristic safely with controlled motions only.
- [ ] GPS obtains an outdoor fix and dashboard hides stale coordinates.

## Dashboard
- [ ] Upload the contents of `data/` to LittleFS using a compatible uploader.
- [ ] Open the IP printed in Serial Monitor from a phone on the same Wi-Fi.
- [ ] Verify WebSocket updates and fallback API.
- [ ] Verify Maps link matches GPS coordinates.

## Alerts
- [ ] Use simulated low thresholds during bench tests; never induce dangerous physiological states.
- [ ] Check buzzer activation/deactivation.
- [ ] Verify Telegram bot/chat configuration and message fields.
- [ ] Confirm alert cooldown prevents flooding.

## Final
- [ ] Secure wiring and enclosure; check battery protection and charging.
- [ ] Document limits: ambient vs body temperature, SpO2 unavailable, heuristic fall detection, GPS coverage.
