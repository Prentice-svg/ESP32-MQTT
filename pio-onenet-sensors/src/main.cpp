#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_AHTX0.h>

#include "app_config.h"

namespace {

WiFiClient wifi_client;
PubSubClient mqtt_client(wifi_client);
Adafruit_AHTX0 aht;

unsigned long last_publish_ms = 0;
bool aht_ready = false;

String make_topic_post()
{
  return String("$sys/") + ONENET_PRODUCT_ID + "/" + ONENET_DEVICE_NAME + "/thing/property/post";
}

String make_topic_set()
{
  return String("$sys/") + ONENET_PRODUCT_ID + "/" + ONENET_DEVICE_NAME + "/thing/property/set";
}

String make_topic_set_reply()
{
  return String("$sys/") + ONENET_PRODUCT_ID + "/" + ONENET_DEVICE_NAME + "/thing/property/set_reply";
}

void connect_wifi()
{
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  Serial.printf("Connecting to Wi-Fi: %s\r\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.printf("\r\nWi-Fi connected, IP=%s\r\n", WiFi.localIP().toString().c_str());
}

void mqtt_callback(char *topic, byte *payload, unsigned int length)
{
  JsonDocument root;
  JsonDocument reply_doc;

  String data;
  data.reserve(length);
  for (unsigned int i = 0; i < length; ++i) {
    data += static_cast<char>(payload[i]);
  }

  Serial.printf("MQTT RX topic=%s payload=%s\r\n", topic, data.c_str());

  DeserializationError err = deserializeJson(root, data);
  if (err) {
    Serial.printf("Cloud JSON parse failed: %s\r\n", err.c_str());
    return;
  }

  const char *msg_id = root["id"] | "0";
  JsonVariantConst params = root["params"];
  if (params.isNull()) {
    return;
  }

  Serial.print("Cloud params -> ");
  serializeJson(params, Serial);
  Serial.print("\r\n");

  reply_doc["id"] = msg_id;
  reply_doc["code"] = 200;
  reply_doc["msg"] = "success";

  char reply_buf[128];
  size_t reply_len = serializeJson(reply_doc, reply_buf, sizeof(reply_buf));
  mqtt_client.publish(make_topic_set_reply().c_str(), reply_buf, reply_len);
}

void connect_mqtt()
{
  if (mqtt_client.connected()) {
    return;
  }

  mqtt_client.setServer(ONENET_MQTT_HOST, ONENET_MQTT_PORT);
  mqtt_client.setCallback(mqtt_callback);

  Serial.println("Connecting to OneNet MQTT...");
  while (!mqtt_client.connected()) {
    if (mqtt_client.connect(ONENET_DEVICE_NAME, ONENET_PRODUCT_ID, ONENET_DEVICE_TOKEN)) {
      Serial.println("MQTT connected");
      mqtt_client.subscribe(make_topic_set().c_str());
      Serial.printf("Subscribed: %s\r\n", make_topic_set().c_str());
    } else {
      Serial.printf("MQTT connect failed, rc=%d\r\n", mqtt_client.state());
      delay(2000);
    }
  }
}

int read_soil_percent()
{
  int raw = analogRead(SOIL_SENSOR_PIN);
  int percent = map(raw, SOIL_ADC_DRY, SOIL_ADC_WET, 0, 100);
  return constrain(percent, 0, 100);
}

bool read_aht(float &temperature, float &humidity)
{
  if (!aht_ready) {
    return false;
  }

  sensors_event_t humidity_event;
  sensors_event_t temperature_event;
  aht.getEvent(&humidity_event, &temperature_event);

  temperature = temperature_event.temperature;
  humidity = humidity_event.relative_humidity;
  return true;
}

void publish_telemetry()
{
  float temperature = NAN;
  float humidity = NAN;
  int soil_percent = read_soil_percent();

  JsonDocument payload_doc;
  char payload_buf[256];

  payload_doc["id"] = String(millis());
  payload_doc["version"] = "1.0";
  JsonObject params = payload_doc["params"].to<JsonObject>();

  if (read_aht(temperature, humidity)) {
    params[PROP_TEMPERATURE]["value"] = temperature;
    params[PROP_HUMIDITY]["value"] = humidity;
  }

  params[PROP_SOIL_MOISTURE]["value"] = soil_percent;

  size_t payload_len = serializeJson(payload_doc, payload_buf, sizeof(payload_buf));
  bool ok = mqtt_client.publish(make_topic_post().c_str(), payload_buf, payload_len);

  Serial.printf("Publish %s\r\n", ok ? "OK" : "FAILED");
  Serial.printf("Soil raw/percent: %d%%\r\n", soil_percent);
  if (!isnan(temperature) && !isnan(humidity)) {
    Serial.printf("AHT30 temperature=%.2f humidity=%.2f\r\n", temperature, humidity);
  } else {
    Serial.println("AHT30 data unavailable");
  }
  Serial.printf("Payload: %s\r\n", payload_buf);
}

}  // namespace

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("ESP32-C3 OneNet sensor node starting...");

  analogReadResolution(12);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  aht_ready = aht.begin(&Wire);
  Serial.printf("AHT30 init: %s\r\n", aht_ready ? "OK" : "FAILED");

  connect_wifi();
  connect_mqtt();
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED) {
    connect_wifi();
  }

  if (!mqtt_client.connected()) {
    connect_mqtt();
  }

  mqtt_client.loop();

  const unsigned long now = millis();
  if (now - last_publish_ms >= TELEMETRY_INTERVAL_MS) {
    last_publish_ms = now;
    publish_telemetry();
  }
}
