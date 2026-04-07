# STM32 <-> ESP32-C3 串口协议

这个项目里，`ESP32-C3` 充当联网协处理器，`STM32F103C8T6` 充当主控制器。

职责分工如下：

- `STM32` 负责采集传感器、执行继电器/LED/电机等控制
- `ESP32-C3` 负责 Wi-Fi、MQTT、OneNet 上云、云端指令收发
- 两者通过 `UART` 交换数据

## 1. 电气连接

- `STM32 TX -> ESP32-C3 RX`
- `STM32 RX -> ESP32-C3 TX`
- `STM32 GND -> ESP32-C3 GND`
- 两边都必须使用 `3.3V TTL`

当前示例默认使用：

- `ESP32-C3 UART1 TX = GPIO5`
- `ESP32-C3 UART1 RX = GPIO4`
- 波特率 `115200`

如果你的硬件引脚不同，修改 [`main/app_main.c`](E:/Dev/物联网/ESP32-MQTT/main/app_main.c) 顶部这些宏即可：

- `UART_TX_PIN`
- `UART_RX_PIN`
- `UART_BAUD_RATE`

## 2. 帧格式

每一帧的格式如下：

```text
+----------+----------+---------+------+--------+--------+-----------+----------+
| Header1  | Header2  | Version | Cmd  | Len_L  | Len_H  | Payload   | Checksum |
+----------+----------+---------+------+--------+--------+-----------+----------+
| 0x55     | 0xAA     | 0x01    | 1B   | 1B     | 1B     | N bytes   | 1B       |
+----------+----------+---------+------+--------+--------+-----------+----------+
```

字段说明：

- `Header1 = 0x55`
- `Header2 = 0xAA`
- `Version = 0x01`
- `Cmd` 命令字
- `Len = Payload` 长度，低字节在前，高字节在后
- `Checksum = Version ^ Cmd ^ Len_L ^ Len_H ^ Payload[0] ^ ...`

最大 `Payload` 长度为 `512` 字节。

## 3. 命令定义

- `0x01` `CMD_TELEMETRY_UPLOAD`
  STM32 -> ESP32-C3
  上传传感器数据，`Payload` 为 UTF-8 JSON

- `0x02` `CMD_PING`
  STM32 -> ESP32-C3
  心跳/连通性检测，可不带 payload

- `0x81` `CMD_ACK`
  ESP32-C3 -> STM32
  应答帧，`Payload` 为 JSON，例如 `{"code":0,"msg":"uploaded"}`

- `0x82` `CMD_CLOUD_PROPERTY_SET`
  ESP32-C3 -> STM32
  云端下发属性控制，`Payload` 为 OneNet `params` 字段对应的 JSON

## 4. STM32 上报数据格式

建议 STM32 发这样的 JSON：

```json
{
  "EnvironmentTemperature": 25.6,
  "EnvironmentHumidity": 61.2,
  "LED": 1
}
```

ESP32-C3 收到后会自动转换为 OneNet 需要的 property post 结构：

```json
{
  "id": "123456",
  "version": "1.0",
  "params": {
    "EnvironmentTemperature": { "value": 25.6 },
    "EnvironmentHumidity": { "value": 61.2 },
    "LED": { "value": 1 }
  }
}
```

注意：

- JSON 里的键名要和你在 OneNet 物模型里的属性标识符一致
- 值支持 `number / bool / string`
- 如果 STM32 直接传入这种结构：

```json
{
  "EnvironmentTemperature": { "value": 25.6 }
}
```

ESP32-C3 也会直接透传该属性结构

## 5. 云端下发格式

当 OneNet 下发：

```json
{
  "id": "8",
  "params": {
    "LED": true
  }
}
```

ESP32-C3 会自动：

1. 取出 `params`
2. 通过 UART 发送 `0x82` 帧给 STM32
3. `Payload` 内容为：

```json
{
  "LED": true
}
```

4. 同时自动给 OneNet 回复：

```json
{
  "id": "8",
  "code": 200,
  "msg": "success"
}
```

## 6. ACK code 说明

- `0` 成功
- `1` MQTT 尚未连接
- `2` 上报 JSON 非法
- `3` MQTT 发布失败
- `4` payload 过大
- `5` 未知命令
- `6` 校验失败

## 7. 推荐调试顺序

1. 先单独验证 ESP32-C3 能连上 Wi-Fi 和 OneNet
2. STM32 先发 `PING`
3. 再发一个最小 telemetry JSON
4. 在 OneNet 里确认属性上报成功
5. 最后再测试云端属性下发到 STM32
