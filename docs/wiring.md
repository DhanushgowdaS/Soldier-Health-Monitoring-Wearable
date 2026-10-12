# Wiring Guide

This table matches the pin assignments in `firmware/SoldierMonitor.ino` for a classic ESP32 DevKit. Confirm the labels and supply requirements on your exact sensor breakout before wiring.

## Pin map

| Module | Signal | ESP32 connection | Notes |
|---|---|---|---|
| Shared I²C bus | SDA | GPIO 21 | LCD, MAX30102, motion module |
| Shared I²C bus | SCL | GPIO 22 | LCD, MAX30102, motion module |
| DHT11 (configured by default) | DATA | GPIO 4 | Change `DHT_TYPE` to `DHT22` for a DHT22 |
| GPS module | TX → ESP32 RX2 | GPIO 16 | Firmware UART2 at 9600 baud |
| GPS module | RX ← ESP32 TX2 | GPIO 17 | Optional if only reading GPS data |
| Buzzer driver | IN | GPIO 25 | Use a transistor/MOSFET driver if current exceeds safe GPIO limits |
| LCD backpack | SDA/SCL | GPIO 21/22 | Firmware LCD address is `0x27` |
| MAX30102 | SDA/SCL | GPIO 21/22 | Tested I²C address `0x57` |
| Motion module | SDA/SCL | GPIO 21/22 | Tested I²C address `0x68`; WHO_AM_I returned `0x70` |

## Electrical precautions

- Connect all module grounds to a common ground.
- ESP32 GPIO pins are **not 5 V tolerant**.
- MAX30102 and motion breakout power requirements vary by board. Follow the specific breakout documentation; do not assume every board has the same regulator/level shifting.
- Some LCD I²C backpacks pull SDA/SCL up to 5 V when powered at 5 V. Verify I²C pull-up voltage or use a suitable bidirectional level shifter before connecting to ESP32 pins.
- Use a stable supply that can handle ESP32 Wi-Fi current peaks. Do not drive a high-current buzzer directly from a GPIO.
- Avoid loose wires and short circuits. Disconnect power before changing wiring.

## I²C verification

An I²C scan of the tested setup found:

- `0x27` — LCD
- `0x57` — MAX30102
- `0x68` — motion sensor

Addresses can vary by hardware. An address scan confirms that a device acknowledges on the bus; it does not by itself prove that every sensor function works.

## Important measurement note

The configured DHT sensor measures **ambient air temperature and relative humidity**, not human body/core temperature.
