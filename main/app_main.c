/* MQTT + UART gateway example for ESP32-C3
 *
 * ESP32-C3 responsibilities:
 * - Connect to Wi-Fi
 * - Connect to OneNet over MQTT
 * - Receive telemetry frames from STM32 over UART
 * - Convert telemetry JSON into OneNet property-post payloads
 * - Forward OneNet property-set commands back to STM32 over UART
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#define MQTT_BROKER_URI         CONFIG_EXAMPLE_BROKER_URL
#define ONENET_PRODUCT_ID       CONFIG_EXAMPLE_ONENET_PRODUCT_ID
#define ONENET_DEVICE_NAME      CONFIG_EXAMPLE_ONENET_DEVICE_NAME
#define ONENET_DEVICE_TOKEN     CONFIG_EXAMPLE_ONENET_DEVICE_TOKEN

#define UART_PORT_NUM           UART_NUM_1
#define UART_BAUD_RATE          115200
#define UART_TX_PIN             GPIO_NUM_5
#define UART_RX_PIN             GPIO_NUM_4
#define UART_RX_BUFFER_SIZE     1024
#define UART_EVENT_QUEUE_SIZE   8

#define FRAME_HEADER_1          0x55
#define FRAME_HEADER_2          0xAA
#define FRAME_VERSION           0x01
#define FRAME_MAX_PAYLOAD_LEN   512

#define CMD_TELEMETRY_UPLOAD    0x01
#define CMD_PING                0x02
#define CMD_ACK                 0x81
#define CMD_CLOUD_PROPERTY_SET  0x82

typedef enum {
    PARSE_WAIT_HEADER_1 = 0,
    PARSE_WAIT_HEADER_2,
    PARSE_WAIT_VERSION,
    PARSE_WAIT_CMD,
    PARSE_WAIT_LEN_LOW,
    PARSE_WAIT_LEN_HIGH,
    PARSE_WAIT_PAYLOAD,
    PARSE_WAIT_CHECKSUM,
} frame_parse_state_t;

static const char *TAG = "mqtt_uart_gateway";

static esp_mqtt_client_handle_t s_mqtt_client;
static volatile bool s_mqtt_connected;
static QueueHandle_t s_uart_event_queue;

static char s_topic_post[96];
static char s_topic_post_reply[112];
static char s_topic_set[96];
static char s_topic_set_reply[112];

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static uint8_t frame_checksum(uint8_t version, uint8_t cmd, uint16_t payload_len, const uint8_t *payload)
{
    uint8_t checksum = 0;

    checksum ^= version;
    checksum ^= cmd;
    checksum ^= (uint8_t)(payload_len & 0xFF);
    checksum ^= (uint8_t)((payload_len >> 8) & 0xFF);
    for (uint16_t i = 0; i < payload_len; ++i) {
        checksum ^= payload[i];
    }

    return checksum;
}

static void init_onenet_topics(void)
{
    snprintf(s_topic_post, sizeof(s_topic_post),
             "$sys/%s/%s/thing/property/post", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    snprintf(s_topic_post_reply, sizeof(s_topic_post_reply),
             "$sys/%s/%s/thing/property/post/reply", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    snprintf(s_topic_set, sizeof(s_topic_set),
             "$sys/%s/%s/thing/property/set", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
    snprintf(s_topic_set_reply, sizeof(s_topic_set_reply),
             "$sys/%s/%s/thing/property/set_reply", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
}

static esp_err_t uart_send_frame(uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frame[FRAME_MAX_PAYLOAD_LEN + 7];
    int index = 0;

    if (payload_len > FRAME_MAX_PAYLOAD_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    frame[index++] = FRAME_HEADER_1;
    frame[index++] = FRAME_HEADER_2;
    frame[index++] = FRAME_VERSION;
    frame[index++] = cmd;
    frame[index++] = (uint8_t)(payload_len & 0xFF);
    frame[index++] = (uint8_t)((payload_len >> 8) & 0xFF);

    if (payload_len > 0 && payload != NULL) {
        memcpy(&frame[index], payload, payload_len);
        index += payload_len;
    }

    frame[index++] = frame_checksum(FRAME_VERSION, cmd, payload_len, payload);

    int written = uart_write_bytes(UART_PORT_NUM, (const char *)frame, index);
    if (written != index) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void uart_send_ack(int code, const char *msg)
{
    char ack_json[96];
    int len = snprintf(ack_json, sizeof(ack_json), "{\"code\":%d,\"msg\":\"%s\"}", code, msg);
    if (len < 0) {
        return;
    }

    if (len >= (int)sizeof(ack_json)) {
        len = sizeof(ack_json) - 1;
    }
    uart_send_frame(CMD_ACK, (const uint8_t *)ack_json, (uint16_t)len);
}

static char *copy_event_data(const char *data, int len)
{
    char *buffer = malloc((size_t)len + 1);
    if (buffer == NULL) {
        return NULL;
    }

    memcpy(buffer, data, (size_t)len);
    buffer[len] = '\0';
    return buffer;
}

static bool topic_matches(const esp_mqtt_event_handle_t event, const char *topic)
{
    size_t topic_len = strlen(topic);
    return event->topic_len == (int)topic_len && memcmp(event->topic, topic, topic_len) == 0;
}

static cJSON *wrap_property_value(const cJSON *src)
{
    cJSON *wrapped = cJSON_CreateObject();
    cJSON *value_item = cJSON_Duplicate(src, true);

    if (wrapped == NULL || value_item == NULL) {
        cJSON_Delete(wrapped);
        cJSON_Delete(value_item);
        return NULL;
    }

    cJSON_AddItemToObject(wrapped, "value", value_item);
    return wrapped;
}

static char *build_onenet_post_payload(const char *telemetry_json)
{
    cJSON *input_root = cJSON_Parse(telemetry_json);
    cJSON *post_root = NULL;
    cJSON *params = NULL;
    char id_buffer[24];
    char *payload = NULL;

    if (input_root == NULL || !cJSON_IsObject(input_root)) {
        goto cleanup;
    }

    post_root = cJSON_CreateObject();
    params = cJSON_CreateObject();
    if (post_root == NULL || params == NULL) {
        goto cleanup;
    }

    snprintf(id_buffer, sizeof(id_buffer), "%" PRIu64, (uint64_t)(esp_timer_get_time() / 1000ULL));
    cJSON_AddStringToObject(post_root, "id", id_buffer);
    cJSON_AddStringToObject(post_root, "version", "1.0");
    cJSON_AddItemToObject(post_root, "params", params);

    for (cJSON *item = input_root->child; item != NULL; item = item->next) {
        cJSON *property = NULL;

        if (item->string == NULL) {
            continue;
        }

        if (cJSON_IsObject(item) && cJSON_GetObjectItem(item, "value") != NULL) {
            property = cJSON_Duplicate(item, true);
        } else if (cJSON_IsNumber(item) || cJSON_IsBool(item) || cJSON_IsString(item)) {
            property = wrap_property_value(item);
        } else {
            ESP_LOGW(TAG, "Skip unsupported telemetry field: %s", item->string);
            continue;
        }

        if (property == NULL) {
            goto cleanup;
        }

        cJSON_AddItemToObject(params, item->string, property);
    }

    if (params->child == NULL) {
        ESP_LOGW(TAG, "Telemetry JSON contains no supported fields");
        goto cleanup;
    }

    payload = cJSON_PrintUnformatted(post_root);

cleanup:
    cJSON_Delete(input_root);
    cJSON_Delete(post_root);
    return payload;
}

static void publish_telemetry_to_onenet(const char *telemetry_json)
{
    char *post_payload = NULL;
    int msg_id;

    if (!s_mqtt_connected || s_mqtt_client == NULL) {
        ESP_LOGW(TAG, "MQTT not ready, drop telemetry: %s", telemetry_json);
        uart_send_ack(1, "mqtt_not_ready");
        return;
    }

    post_payload = build_onenet_post_payload(telemetry_json);
    if (post_payload == NULL) {
        ESP_LOGE(TAG, "Failed to build OneNet payload from telemetry");
        uart_send_ack(2, "invalid_json");
        return;
    }

    msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_post, post_payload, 0, 1, 0);
    if (msg_id >= 0) {
        ESP_LOGI(TAG, "Telemetry uploaded, msg_id=%d", msg_id);
        uart_send_ack(0, "uploaded");
    } else {
        ESP_LOGE(TAG, "Telemetry publish failed");
        uart_send_ack(3, "publish_failed");
    }

    free(post_payload);
}

static void forward_property_set_to_stm32(const char *cloud_payload)
{
    cJSON *root = cJSON_Parse(cloud_payload);
    cJSON *params = NULL;
    char *params_json = NULL;

    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse cloud property-set payload");
        return;
    }

    params = cJSON_GetObjectItem(root, "params");
    if (params == NULL) {
        ESP_LOGW(TAG, "Cloud payload does not contain params");
        goto cleanup;
    }

    params_json = cJSON_PrintUnformatted(params);
    if (params_json == NULL) {
        goto cleanup;
    }

    if (strlen(params_json) > FRAME_MAX_PAYLOAD_LEN) {
        ESP_LOGW(TAG, "Cloud params exceed UART payload limit");
        goto cleanup;
    }

    if (uart_send_frame(CMD_CLOUD_PROPERTY_SET, (const uint8_t *)params_json, (uint16_t)strlen(params_json)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to forward property set to STM32");
    } else {
        ESP_LOGI(TAG, "Forwarded property set to STM32: %s", params_json);
    }

cleanup:
    free(params_json);
    cJSON_Delete(root);
}

static void reply_property_set(const char *cloud_payload)
{
    cJSON *root = cJSON_Parse(cloud_payload);
    cJSON *id = NULL;
    char reply_data[128];
    int msg_id;

    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse property-set payload for reply");
        return;
    }

    id = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsString(id) || id->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }

    snprintf(reply_data, sizeof(reply_data),
             "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\"}",
             id->valuestring);
    msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_set_reply, reply_data, 0, 1, 0);
    ESP_LOGI(TAG, "sent property-set reply, msg_id=%d", msg_id);
    cJSON_Delete(root);
}

static void handle_uart_protocol_frame(uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    char payload_text[FRAME_MAX_PAYLOAD_LEN + 1];

    if (payload_len > FRAME_MAX_PAYLOAD_LEN) {
        uart_send_ack(4, "payload_too_large");
        return;
    }

    memcpy(payload_text, payload, payload_len);
    payload_text[payload_len] = '\0';

    switch (cmd) {
    case CMD_TELEMETRY_UPLOAD:
        ESP_LOGI(TAG, "UART telemetry: %s", payload_text);
        publish_telemetry_to_onenet(payload_text);
        break;
    case CMD_PING:
        uart_send_ack(0, "pong");
        break;
    default:
        ESP_LOGW(TAG, "Unknown UART command: 0x%02X", cmd);
        uart_send_ack(5, "unknown_cmd");
        break;
    }
}

static void uart_rx_task(void *arg)
{
    uart_event_t event;
    uint8_t data[128];
    frame_parse_state_t state = PARSE_WAIT_HEADER_1;
    uint8_t version = 0;
    uint8_t cmd = 0;
    uint16_t payload_len = 0;
    uint16_t payload_index = 0;
    uint8_t payload[FRAME_MAX_PAYLOAD_LEN];

    while (true) {
        if (xQueueReceive(s_uart_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event.type == UART_DATA) {
            int read_len = uart_read_bytes(UART_PORT_NUM, data, sizeof(data), pdMS_TO_TICKS(100));
            for (int i = 0; i < read_len; ++i) {
                uint8_t byte = data[i];

                switch (state) {
                case PARSE_WAIT_HEADER_1:
                    if (byte == FRAME_HEADER_1) {
                        state = PARSE_WAIT_HEADER_2;
                    }
                    break;
                case PARSE_WAIT_HEADER_2:
                    if (byte == FRAME_HEADER_2) {
                        state = PARSE_WAIT_VERSION;
                    } else {
                        state = PARSE_WAIT_HEADER_1;
                    }
                    break;
                case PARSE_WAIT_VERSION:
                    version = byte;
                    if (version == FRAME_VERSION) {
                        state = PARSE_WAIT_CMD;
                    } else {
                        state = PARSE_WAIT_HEADER_1;
                    }
                    break;
                case PARSE_WAIT_CMD:
                    cmd = byte;
                    state = PARSE_WAIT_LEN_LOW;
                    break;
                case PARSE_WAIT_LEN_LOW:
                    payload_len = byte;
                    state = PARSE_WAIT_LEN_HIGH;
                    break;
                case PARSE_WAIT_LEN_HIGH:
                    payload_len |= (uint16_t)(byte << 8);
                    if (payload_len > FRAME_MAX_PAYLOAD_LEN) {
                        ESP_LOGW(TAG, "UART frame too long: %u", payload_len);
                        state = PARSE_WAIT_HEADER_1;
                    } else if (payload_len == 0) {
                        payload_index = 0;
                        state = PARSE_WAIT_CHECKSUM;
                    } else {
                        payload_index = 0;
                        state = PARSE_WAIT_PAYLOAD;
                    }
                    break;
                case PARSE_WAIT_PAYLOAD:
                    payload[payload_index++] = byte;
                    if (payload_index >= payload_len) {
                        state = PARSE_WAIT_CHECKSUM;
                    }
                    break;
                case PARSE_WAIT_CHECKSUM:
                    if (byte == frame_checksum(version, cmd, payload_len, payload)) {
                        handle_uart_protocol_frame(cmd, payload, payload_len);
                    } else {
                        ESP_LOGW(TAG, "UART checksum mismatch");
                        uart_send_ack(6, "bad_checksum");
                    }
                    state = PARSE_WAIT_HEADER_1;
                    break;
                default:
                    state = PARSE_WAIT_HEADER_1;
                    break;
                }
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            ESP_LOGW(TAG, "UART buffer overflow, flushing");
            uart_flush_input(UART_PORT_NUM);
            xQueueReset(s_uart_event_queue);
            state = PARSE_WAIT_HEADER_1;
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        ESP_LOGI(TAG, "subscribe post reply, msg_id=%d",
                 esp_mqtt_client_subscribe(event->client, s_topic_post_reply, 0));
        ESP_LOGI(TAG, "subscribe property set, msg_id=%d",
                 esp_mqtt_client_subscribe(event->client, s_topic_set, 0));
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA: {
        char *topic = copy_event_data(event->topic, event->topic_len);
        char *data = copy_event_data(event->data, event->data_len);

        if (topic == NULL || data == NULL) {
            free(topic);
            free(data);
            break;
        }

        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        ESP_LOGI(TAG, "TOPIC=%s", topic);
        ESP_LOGI(TAG, "DATA=%s", data);

        if (topic_matches(event, s_topic_set)) {
            forward_property_set_to_stm32(data);
            reply_property_set(data);
        }

        free(topic);
        free(data);
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport socket errno", event->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void uart_gateway_start(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_RX_BUFFER_SIZE, UART_RX_BUFFER_SIZE, UART_EVENT_QUEUE_SIZE, &s_uart_event_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(uart_rx_task, "uart_rx_task", 4096, NULL, 10, NULL);
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.username = ONENET_PRODUCT_ID,
        .credentials.client_id = ONENET_DEVICE_NAME,
        .credentials.authentication.password = ONENET_DEVICE_TOKEN,
    };

#if CONFIG_EXAMPLE_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.broker.address.uri, "FROM_STDIN") == 0) {
        int count = 0;

        printf("Please enter url of mqtt broker\n");
        while (count < (int)sizeof(line)) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count++] = (char)c;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        mqtt_cfg.broker.address.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_mqtt_client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);

    init_onenet_topics();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    uart_gateway_start();
    mqtt_app_start();
}
