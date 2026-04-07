#pragma once

// Wi-Fi credentials
#define WIFI_SSID           "CMCC-etru"
#define WIFI_PASSWORD       "4us46hfx"

// OneNet MQTT configuration
#define ONENET_MQTT_HOST    "mqtts.heclouds.com"
#define ONENET_MQTT_PORT    1883
#define ONENET_PRODUCT_ID   "45Z51DlJn3"
#define ONENET_DEVICE_NAME  "esp32-01"
#define ONENET_DEVICE_TOKEN "version=2018-10-31&res=products%2F45Z51DlJn3%2Fdevices%2Fesp32-01&et=1806712140&method=md5&sign=C%2Fy4L5HBQBksjbHUSe9DSw%3D%3D"

// OneNet property identifiers
// These identifiers should match your OneNet model exactly.
#define PROP_TEMPERATURE    "EnvironmentTemperature"
#define PROP_HUMIDITY       "EnvironmentHumidity"
#define PROP_SOIL_MOISTURE  "SoilMoisture"

// Sensor pins
// Change these to match your actual wiring.
#define I2C_SDA_PIN         8
#define I2C_SCL_PIN         9
#define SOIL_SENSOR_PIN     4

// Soil sensor calibration
// Update these after measuring your own dry/wet readings.
#define SOIL_ADC_DRY        3000
#define SOIL_ADC_WET        1200

// Publish interval
#define TELEMETRY_INTERVAL_MS 10000UL
