# ESP32-C3 OneNet Sensor Node (PlatformIO)

This is a standalone PlatformIO project for an ESP32-C3 board.

It reads:

- AHT30 temperature and humidity over I2C
- HW-390 style soil moisture sensor through one ADC pin

It publishes telemetry directly to OneNet over MQTT without STM32.

## Open In VS Code

Open the folder:

`E:\Dev\esp32-mqtt\pio-onenet-sensors`

with the PlatformIO extension in VS Code.

## Before You Build

Edit:

`include/app_config.h`

Update:

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `ONENET_PRODUCT_ID`
- `ONENET_DEVICE_NAME`
- `ONENET_DEVICE_TOKEN`
- property identifiers such as `PROP_SOIL_MOISTURE` if your OneNet model uses a different name
- sensor pins if your wiring differs
- `SOIL_ADC_DRY` and `SOIL_ADC_WET` after calibration

## Default Pin Assumptions

- AHT30 SDA: `GPIO8`
- AHT30 SCL: `GPIO9`
- Soil moisture analog output: `GPIO4`

If your board wiring is different, change the values in `include/app_config.h`.

## OneNet Payload

The project publishes a property post payload like:

```json
{
  "id": "12345",
  "version": "1.0",
  "params": {
    "temperature": { "value": 25.6 },
    "humidity": { "value": 61.2 },
    "soil_moisture": { "value": 48 }
  }
}
```

## Notes

- The board profile is currently `esp32-c3-devkitm-1`, which is a good generic starting point for many ESP32-C3 boards.
- If your ESP32-C3 Super Mini uses a different PlatformIO board definition more reliably, you can change `board` in `platformio.ini`.
