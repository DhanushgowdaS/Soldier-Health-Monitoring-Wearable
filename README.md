# Soldier Health Monitoring Wearable

ESP32-based educational prototype that combines sensor readings, a local dashboard, a small LCD, a buzzer, GPS coordinates, and Telegram notifications.

> **Safety notice:** This is a student prototype, not a medical device or a validated fall-detection system. Sensor estimates can be inaccurate, alarms can be missed or triggered incorrectly, and the project must not be used as the sole basis for real emergency decisions.

## Contents

- [Features](#features)
- [Hardware](#hardware)
- [Repository layout](#repository-layout)
- [Wiring](#wiring)
- [Software requirements](#software-requirements)
- [Configure credentials](#configure-credentials)
- [Build and upload firmware](#build-and-upload-firmware)
- [Upload the dashboard to LittleFS](#upload-the-dashboard-to-littlefs)
- [Use the dashboard](#use-the-dashboard)
- [Telegram alerts](#telegram-alerts)
- [Current behavior and thresholds](#current-behavior-and-thresholds)
- [Troubleshooting](#troubleshooting)
- [Testing checklist](#testing-checklist)
- [Limitations and safety](#limitations-and-safety)
- [Contributing](#contributing)

## Features

- **MAX30102:** optical pulse sampling and a tentative heart-rate estimate; the firmware includes an SpO₂ calculation path, but readings must be treated as experimental and may remain unavailable until enough valid samples are collected.
- **DHT11 by default:** ambient air temperature and relative humidity. It does **not** measure body/core temperature. If using a DHT22, change `DHT_TYPE` in the firmware.
- **Motion sensor:** direct I²C register support for the connected motion module. The module tested in this project returned WHO_AM_I `0x70`; firmware supports IDs `0x70` and `0x68`. Motion status and a heuristic possible-fall alert are provided.
- **GPS:** latitude/longitude when a recent fix is available, plus a Google Maps link.
- **Local outputs:** I²C LCD and buzzer.
- **Local web dashboard:** HTML/CSS/JavaScript stored in ESP32 LittleFS.
- **Live telemetry:** HTTP server on port 80, WebSocket on port 81, and `/api/data` JSON fallback.
- **Telegram:** configurable alert messages with sensor values and a map link when a GPS fix is available.

## Hardware

- Classic ESP32 DevKit (not ESP32-S3)
- MAX30102 optical sensor module
- MPU6050-compatible motion module (tested module reported WHO_AM_I `0x70`)
- DHT11 (or DHT22 after changing the firmware setting)
- NEO-6M-family GPS module
- I²C 16×2 LCD backpack (configured for address `0x27`)
- Buzzer and suitable driver circuit
- Stable power supply, common ground, wiring, and optional enclosure

Module breakout boards differ. Confirm the pin labels and voltage requirements printed on your exact boards before connecting power.

## Repository layout

```text
.
├── README.md
├── LICENSE
├── .gitignore
├── firmware/
│   ├── SoldierMonitor.ino
│   ├── secrets.h.example
│   ├── spo2_algorithm.cpp
│   ├── spo2_algorithm.h
│   └── components_testing_code/
│       └── MAX30102_Test/
│           └── MAX30102_Test.ino
├── data/
│   ├── index.html
│   ├── style.css
│   └── script.js
└── docs/
    ├── architecture.md
    ├── wiring.md
    └── testing.md
```

- `firmware/SoldierMonitor.ino` — main ESP32 sketch.
- `firmware/secrets.h.example` — credential template. Copy it to `firmware/secrets.h` and fill in your own values.
- `data/` — web files served from LittleFS. They must be uploaded to the filesystem separately from the sketch.
- `firmware/components_testing_code/` — standalone component test sketch.
- `docs/` — wiring, architecture, and bring-up checklist.

## Wiring

See **[docs/wiring.md](docs/wiring.md)** for the pin table and electrical precautions.

| Signal/module | ESP32 connection |
|---|---|
| Shared I²C SDA | GPIO 21 |
| Shared I²C SCL | GPIO 22 |
| DHT data | GPIO 4 |
| GPS TX → ESP32 RX2 | GPIO 16 |
| GPS RX ← ESP32 TX2 | GPIO 17 (optional for read-only GPS use) |
| Buzzer driver input | GPIO 25 |
| LCD / MAX30102 / motion module | Shared I²C bus |

The expected I²C addresses observed during testing were LCD `0x27`, MAX30102 `0x57`, and motion module `0x68`. Actual addresses may vary by board/configuration.

## Software requirements

1. Arduino IDE 2.x.
2. ESP32 board support package; select a classic **ESP32 Dev Module** matching your board.
3. Libraries:
   - SparkFun MAX3010x Pulse and Proximity Sensor Library
   - DHT sensor library by Adafruit
   - Adafruit Unified Sensor
   - Adafruit BusIO
   - TinyGPSPlus
   - LiquidCrystal_I2C compatible with your LCD backpack
   - WebSockets by Markus Sattler
   - ArduinoJson 7
4. A compatible LittleFS upload tool for your installed ESP32 board package.

Install library dependencies using Arduino IDE Library Manager where available. If compilation reports a macro conflict, ensure you have copied the current firmware from this repository rather than an older downloaded ZIP.

## Configure credentials

1. Copy `firmware/secrets.h.example` to `firmware/secrets.h`.
2. Edit the local `firmware/secrets.h` and set:
   - `WIFI_SSID`
   - `WIFI_PASSWORD`
   - `TELEGRAM_BOT_TOKEN`
   - `TELEGRAM_CHAT_ID`
3. Keep `firmware/secrets.h` private. It is ignored by Git; **never commit real Wi-Fi passwords or bot tokens**.

If credentials have ever been committed publicly, rotate the affected password/token. The example file contains placeholders only.

## Build and upload firmware

1. Clone or download this repository.
2. Create `firmware/secrets.h` as described above.
3. Open `firmware/SoldierMonitor.ino` in Arduino IDE.
4. Select the correct classic ESP32 board and COM port.
5. Verify/compile, then upload the sketch.
6. Open Serial Monitor at **115200 baud**.
7. Wait for Wi-Fi connection and note the dashboard IP printed to Serial Monitor.

The main sketch expects the accompanying `spo2_algorithm.cpp`, `spo2_algorithm.h`, and `secrets.h` files to be available in the Arduino sketch build. If Arduino IDE reports missing local headers/source files, use the complete firmware folder and arrange the sketch folder so the IDE compiles the associated source files.

## Upload the dashboard to LittleFS

The dashboard is **not embedded in the firmware sketch**. It is served from the ESP32 filesystem.

1. Keep these files in the repository's `data/` directory: `index.html`, `style.css`, and `script.js`.
2. Use a LittleFS uploader compatible with your ESP32 board package.
3. Upload the **contents of the `data/` folder** to the LittleFS filesystem so the filesystem root contains:
   - `/index.html`
   - `/style.css`
   - `/script.js`
4. Restart the ESP32.
5. Connect a phone or computer to the same Wi-Fi network and open `http://<ESP32-IP>/`.
6. After updating dashboard files, hard-refresh the browser (Ctrl+F5 on desktop).

Uploading the firmware sketch does not automatically upload the LittleFS dashboard files. Re-upload LittleFS only when dashboard assets change or the filesystem is missing/out of date.

## Use the dashboard

- Open the IP printed in Serial Monitor.
- Live readings are broadcast over WebSocket port 81.
- `http://<ESP32-IP>/api/data` provides a JSON snapshot for diagnosis.
- If the motion module is unavailable, the dashboard should say so rather than implying fall detection is working.
- A `POSSIBLE FALL` label is a heuristic alert, not a confirmed diagnosis.

## Telegram alerts

- Create a Telegram bot and obtain its token from BotFather.
- Obtain the target chat ID and put both values in your local `firmware/secrets.h`.
- The ESP32 must have Wi-Fi internet access for Telegram delivery.
- Test with harmless environmental or controlled bench conditions. Do not induce dangerous physiological conditions to test alerts.
- The current firmware uses an insecure TLS client configuration for Telegram to simplify prototype connectivity. This does not validate Telegram's TLS certificate and is **not suitable for production/security-sensitive deployment**.

## Current behavior and thresholds

The main firmware currently defines these alert thresholds:

| Signal | Firmware threshold |
|---|---:|
| Heart rate | Above 100 BPM or below 60 BPM when a reading is valid |
| SpO₂ estimate | Below 95% when a reading is valid |
| Ambient temperature | Above 37 °C |
| Relative humidity | Above 85% |
| Motion impact heuristic | Acceleration magnitude at or above 3.0 g, after an 8-second startup guard |
| Possible-fall confirmation | Impact candidate followed by approximately 2 seconds of stillness |
| Possible-fall display hold | 10 seconds, then the demo alert clears automatically |

These values are implementation settings for a prototype, not medical advice or validated thresholds. Movement, mounting, sensor bias, and different modules can change detection behavior. Automatic clearing does not establish that a wearer is safe.

## Troubleshooting

### Dashboard says “MPU6050 unavailable”
- Open Serial Monitor at 115200 baud and check the motion sensor startup message.
- Run an I²C scanner on GPIO 21/22. The tested setup returned `0x68` for the motion module.
- The tested module returned WHO_AM_I `0x70`; the main firmware includes direct register access for this ID and `0x68`.
- If I²C detects the module but the dashboard does not, make sure the latest `SoldierMonitor.ino` was uploaded.

### Dashboard does not update
- Confirm phone/computer and ESP32 are on the same Wi-Fi network.
- Check the IP address printed after Wi-Fi connects.
- Verify LittleFS contains `index.html`, `style.css`, and `script.js` at the filesystem root.
- Try `/api/data` and hard-refresh the dashboard.

### GPS has no fix
- Test outdoors with a clear view of the sky; first fix may take several minutes.
- Confirm GPS TX connects to ESP32 GPIO 16 and grounds are shared.
- Coordinates are only reported when a recent fix is available.

### Compilation errors
- Confirm the selected board is a classic ESP32 Dev Module.
- Install the listed libraries.
- If the compiler points to `DEG_TO_RAD`, you are likely compiling an older sketch: the current source uses `DEGREES_TO_RADIANS`.
- A “library claims to run on all architectures” message is a warning by itself; look for the actual `error:` line.

### Telegram does not send
- Confirm Wi-Fi internet access, bot token, chat ID, and that the bot has been started in the target chat.
- Keep credentials in the ignored local `firmware/secrets.h` only.

## Testing checklist

Use [docs/testing.md](docs/testing.md) to record hardware bring-up, dashboard, GPS, and Telegram checks. Test the motion heuristic with safe, controlled movements only.

## Contributing

Bug reports and pull requests are welcome. Include the ESP32 board selection, Arduino IDE/ESP32 core version, relevant Serial Monitor output, and the steps to reproduce. Never include Wi-Fi passwords, Telegram bot tokens, private chat information, or other secrets in issues or screenshots.
