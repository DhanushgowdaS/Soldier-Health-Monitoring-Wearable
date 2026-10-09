# Wiring Guide

Pin assignments for `firmware/SoldierMonitor.ino`; verify every breakout before powering.

| Module | Signal | ESP32 |
|---|---|---|
| Shared I2C | SDA | GPIO 21 |
| Shared I2C | SCL | GPIO 22 |
| DHT22 | DATA | GPIO 4 |
| GPS | TX -> ESP RX2 | GPIO 16 |
| GPS | RX <- ESP TX2 (optional) | GPIO 17 |
| Active buzzer driver input | IN | GPIO 25 |
| MAX30102 | SDA/SCL | GPIO 21/22 |
| MPU6050 | SDA/SCL | GPIO 21/22 |
| I2C LCD | SDA/SCL | GPIO 21/22 |

- Join grounds. ESP32 GPIO is not 5 V tolerant.
- MAX30102 and MPU6050 breakouts should use a supply compatible with their board, typically 3.3 V.
- Some LCD backpacks pull I2C up to 5 V. Use a bidirectional level shifter or verified 3.3 V pull-ups before attaching to ESP32.
- Use a stable 3.3 V regulator capable of Wi-Fi current peaks. Do not power a high-current buzzer directly from GPIO; use a transistor/MOSFET driver if required.
- Typical I2C addresses: MAX30102 0x57, MPU6050 0x68, LCD 0x27 or 0x3F.
- DHT22 measures ambient temperature/humidity, not body temperature.
