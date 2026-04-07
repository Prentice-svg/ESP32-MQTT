#pragma once

// Wi-Fi credentials
#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"

// OneNet MQTT configuration
#define ONENET_MQTT_HOST    "mqtts.heclouds.com"
#define ONENET_MQTT_PORT    1883
#define ONENET_PRODUCT_ID   "YOUR_PRODUCT_ID"
#define ONENET_DEVICE_NAME  "YOUR_DEVICE_NAME"
#define ONENET_DEVICE_TOKEN "YOUR_DEVICE_TOKEN"

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
