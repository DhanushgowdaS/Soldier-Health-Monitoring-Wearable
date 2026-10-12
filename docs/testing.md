# Hardware Bring-up and Testing Checklist

Record the test date, board/core versions, results, and any Serial Monitor output when testing. Do not publish credentials or private location data in screenshots/logs.

## 1. Power and wiring

- [ ] Verify the ESP32 board is the classic ESP32 DevKit, not ESP32-S3.
- [ ] Verify supply voltage for each breakout and common ground.
- [ ] Check that I²C pull-ups do not drive ESP32 SDA/SCL above 3.3 V.
- [ ] Inspect for shorts, loose wires, and unsafe battery connections.

## 2. I²C bus

- [ ] Run an I²C scanner on GPIO 21/22.
- [ ] Confirm expected devices appear. Tested setup: LCD `0x27`, MAX30102 `0x57`, motion module `0x68`.
- [ ] Note: the tested motion module returned WHO_AM_I `0x70`; the main firmware should report a motion sensor detected message for this ID.
- [ ] Confirm LCD initializes without disrupting the shared bus.

## 3. Sensors

- [ ] DHT readings update every few seconds. Verify the configured type matches the installed DHT11/DHT22.
- [ ] MAX30102 reports optical signal when a finger is placed gently on the sensor; readings may fluctuate with motion.
- [ ] Motion activity changes between stationary and movement when the sensor is gently moved.
- [ ] Test the possible-fall heuristic only with safe, controlled sensor movements; do not drop a person or create a hazardous test.
- [ ] Confirm a possible-fall demo alert clears after its configured 10-second hold.
- [ ] Test GPS outdoors with a clear view of the sky; verify valid coordinates and map link before trusting location output.

## 4. Firmware and dashboard

- [ ] Compile `firmware/SoldierMonitor.ino` with the required libraries and local `firmware/secrets.h`.
- [ ] Upload firmware to the correct COM port.
- [ ] Open Serial Monitor at 115200 baud and save relevant startup diagnostics.
- [ ] Upload the contents of `data/` to LittleFS root.
- [ ] Open the ESP32 IP from a browser on the same Wi-Fi.
- [ ] Verify `/api/data` returns JSON.
- [ ] Confirm WebSocket telemetry updates the dashboard and hard-refresh after asset changes.
- [ ] Confirm dashboard labels sensor-unavailable states instead of implying the sensor is working.

## 5. Alerts

- [ ] Verify buzzer output using safe bench tests.
- [ ] Configure Telegram credentials locally and confirm the bot can send to the intended chat.
- [ ] Verify the map link appears only when a recent GPS fix is available.
- [ ] Check that repeated sensor values do not cause unexpected message flooding.
- [ ] Never create dangerous physiological conditions to test heart-rate or SpO₂ alerts.

## 6. Release checklist

- [ ] No credentials or private location information are committed.
- [ ] README, wiring guide, architecture, and limitations match the current firmware.
- [ ] All modified files are uploaded to the correct repository and branch.
- [ ] The project is clearly labelled as an educational prototype, not a medical device.
