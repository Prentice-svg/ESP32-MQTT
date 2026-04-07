#include "usart.h"
#include "cJSON.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define ESP_FRAME_HEADER_1        0x55
#define ESP_FRAME_HEADER_2        0xAA
#define ESP_FRAME_VERSION         0x01

#define ESP_CMD_TELEMETRY_UPLOAD  0x01
#define ESP_CMD_PING              0x02
#define ESP_CMD_ACK               0x81
#define ESP_CMD_CLOUD_SET         0x82

#define ESP_UART_TIMEOUT_MS       100
#define ESP_FRAME_MAX_PAYLOAD     512

typedef enum {
    ESP_RX_WAIT_HEADER_1 = 0,
    ESP_RX_WAIT_HEADER_2,
    ESP_RX_WAIT_VERSION,
    ESP_RX_WAIT_CMD,
    ESP_RX_WAIT_LEN_LOW,
    ESP_RX_WAIT_LEN_HIGH,
    ESP_RX_WAIT_PAYLOAD,
    ESP_RX_WAIT_CHECKSUM
} ESP_RxState;

static uint8_t g_esp_rx_byte;

static void APP_SetLed(uint8_t on)
{
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void APP_HandleAckJson(const char *json_text)
{
    cJSON *root = cJSON_Parse(json_text);
    cJSON *code;
    cJSON *msg;

    if (root == NULL) {
        return;
    }

    code = cJSON_GetObjectItem(root, "code");
    msg = cJSON_GetObjectItem(root, "msg");

    if (cJSON_IsNumber(code) && cJSON_IsString(msg)) {
        printf("ESP ACK: code=%d msg=%s\r\n", code->valueint, msg->valuestring);
    }

    cJSON_Delete(root);
}

static void APP_HandleCloudSetJson(const char *json_text)
{
    cJSON *root = cJSON_Parse(json_text);
    cJSON *led;

    if (root == NULL) {
        printf("Cloud set parse failed\r\n");
        return;
    }

    led = cJSON_GetObjectItem(root, "LED");
    if (led == NULL) {
        led = cJSON_GetObjectItem(root, "led");
    }

    if (cJSON_IsBool(led)) {
        APP_SetLed(cJSON_IsTrue(led) ? 1U : 0U);
        printf("Cloud set LED=%d\r\n", cJSON_IsTrue(led) ? 1 : 0);
    } else if (cJSON_IsNumber(led)) {
        APP_SetLed(led->valueint ? 1U : 0U);
        printf("Cloud set LED=%d\r\n", led->valueint ? 1 : 0);
    }

    cJSON_Delete(root);
}

static uint8_t esp_frame_checksum(uint8_t version, uint8_t cmd, uint16_t payload_len, const uint8_t *payload)
{
    uint8_t checksum = 0;
    uint16_t i;

    checksum ^= version;
    checksum ^= cmd;
    checksum ^= (uint8_t)(payload_len & 0xFF);
    checksum ^= (uint8_t)((payload_len >> 8) & 0xFF);
    for (i = 0; i < payload_len; ++i) {
        checksum ^= payload[i];
    }

    return checksum;
}

static HAL_StatusTypeDef esp_uart_send_frame(UART_HandleTypeDef *huart, uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    uint8_t frame[ESP_FRAME_MAX_PAYLOAD + 7];
    uint16_t index = 0;

    if (payload_len > ESP_FRAME_MAX_PAYLOAD) {
        return HAL_ERROR;
    }

    frame[index++] = ESP_FRAME_HEADER_1;
    frame[index++] = ESP_FRAME_HEADER_2;
    frame[index++] = ESP_FRAME_VERSION;
    frame[index++] = cmd;
    frame[index++] = (uint8_t)(payload_len & 0xFF);
    frame[index++] = (uint8_t)((payload_len >> 8) & 0xFF);

    if (payload_len > 0 && payload != NULL) {
        memcpy(&frame[index], payload, payload_len);
        index += payload_len;
    }

    frame[index++] = esp_frame_checksum(ESP_FRAME_VERSION, cmd, payload_len, payload);

    return HAL_UART_Transmit(huart, frame, index, ESP_UART_TIMEOUT_MS);
}

HAL_StatusTypeDef ESP_SendPing(UART_HandleTypeDef *huart)
{
    return esp_uart_send_frame(huart, ESP_CMD_PING, NULL, 0);
}

HAL_StatusTypeDef ESP_SendTelemetry(UART_HandleTypeDef *huart, float temperature, float humidity, uint8_t led)
{
    char json[160];
    int len = snprintf(
        json,
        sizeof(json),
        "{\"EnvironmentTemperature\":%.2f,\"EnvironmentHumidity\":%.2f,\"LED\":%u}",
        temperature,
        humidity,
        led
    );

    if (len <= 0 || len >= (int)sizeof(json)) {
        return HAL_ERROR;
    }

    return esp_uart_send_frame(huart, ESP_CMD_TELEMETRY_UPLOAD, (const uint8_t *)json, (uint16_t)len);
}

static void ESP_HandleReceivedFrame(uint8_t cmd, const uint8_t *payload, uint16_t payload_len)
{
    char json_text[ESP_FRAME_MAX_PAYLOAD + 1];

    if (payload_len > ESP_FRAME_MAX_PAYLOAD) {
        return;
    }

    memcpy(json_text, payload, payload_len);
    json_text[payload_len] = '\0';

    if (cmd == ESP_CMD_ACK) {
        APP_HandleAckJson(json_text);
    } else if (cmd == ESP_CMD_CLOUD_SET) {
        APP_HandleCloudSetJson(json_text);
    }
}

void ESP_ProcessReceivedByte(uint8_t byte)
{
    static ESP_RxState state = ESP_RX_WAIT_HEADER_1;
    static uint8_t version = 0;
    static uint8_t cmd = 0;
    static uint16_t payload_len = 0;
    static uint16_t payload_index = 0;
    static uint8_t payload[ESP_FRAME_MAX_PAYLOAD];

    switch (state) {
    case ESP_RX_WAIT_HEADER_1:
        if (byte == ESP_FRAME_HEADER_1) {
            state = ESP_RX_WAIT_HEADER_2;
        }
        break;
    case ESP_RX_WAIT_HEADER_2:
        state = (byte == ESP_FRAME_HEADER_2) ? ESP_RX_WAIT_VERSION : ESP_RX_WAIT_HEADER_1;
        break;
    case ESP_RX_WAIT_VERSION:
        version = byte;
        state = (version == ESP_FRAME_VERSION) ? ESP_RX_WAIT_CMD : ESP_RX_WAIT_HEADER_1;
        break;
    case ESP_RX_WAIT_CMD:
        cmd = byte;
        state = ESP_RX_WAIT_LEN_LOW;
        break;
    case ESP_RX_WAIT_LEN_LOW:
        payload_len = byte;
        state = ESP_RX_WAIT_LEN_HIGH;
        break;
    case ESP_RX_WAIT_LEN_HIGH:
        payload_len |= (uint16_t)(byte << 8);
        if (payload_len > ESP_FRAME_MAX_PAYLOAD) {
            state = ESP_RX_WAIT_HEADER_1;
        } else if (payload_len == 0) {
            payload_index = 0;
            state = ESP_RX_WAIT_CHECKSUM;
        } else {
            payload_index = 0;
            state = ESP_RX_WAIT_PAYLOAD;
        }
        break;
    case ESP_RX_WAIT_PAYLOAD:
        payload[payload_index++] = byte;
        if (payload_index >= payload_len) {
            state = ESP_RX_WAIT_CHECKSUM;
        }
        break;
    case ESP_RX_WAIT_CHECKSUM:
        if (byte == esp_frame_checksum(version, cmd, payload_len, payload)) {
            ESP_HandleReceivedFrame(cmd, payload, payload_len);
        } else {
            printf("ESP frame checksum error\r\n");
        }
        state = ESP_RX_WAIT_HEADER_1;
        break;
    default:
        state = ESP_RX_WAIT_HEADER_1;
        break;
    }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1) {
        ESP_ProcessReceivedByte(g_esp_rx_byte);
        HAL_UART_Receive_IT(&huart1, &g_esp_rx_byte, 1);
    }
}

void ESP_StartReceiveIT(void)
{
    HAL_UART_Receive_IT(&huart1, &g_esp_rx_byte, 1);
}

void Application_Loop(void)
{
    float temperature = 26.5f;
    float humidity = 62.3f;
    uint8_t led_state = 1;

    ESP_SendPing(&huart1);
    HAL_Delay(100);

    ESP_SendTelemetry(&huart1, temperature, humidity, led_state);
    HAL_Delay(5000);
}
