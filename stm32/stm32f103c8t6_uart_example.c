#include "usart.h"
#include <stdint.h>
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
