# STM32 Integration Notes

This directory intentionally contains only notes, not STM32 source code.

Use your own STM32 project to implement the UART protocol used by the ESP32-C3 firmware in this repository.

## UART frame format

```text
+----------+----------+---------+------+--------+--------+-----------+----------+
| Header1  | Header2  | Version | Cmd  | Len_L  | Len_H  | Payload   | Checksum |
+----------+----------+---------+------+--------+--------+-----------+----------+
| 0x55     | 0xAA     | 0x01    | 1B   | 1B     | 1B     | N bytes   | 1B       |
+----------+----------+---------+------+--------+--------+-----------+----------+
```

- `Header1 = 0x55`
- `Header2 = 0xAA`
- `Version = 0x01`
- `Len` is payload length in little-endian order
- `Checksum = Version ^ Cmd ^ Len_L ^ Len_H ^ Payload[0] ^ ...`

## Commands

- `0x01`: telemetry upload, STM32 -> ESP32-C3
- `0x02`: ping, STM32 -> ESP32-C3
- `0x81`: ack, ESP32-C3 -> STM32
- `0x82`: cloud property set, ESP32-C3 -> STM32

## Telemetry JSON example

Match JSON key names to your OneNet property identifiers exactly.

```json
{
  "temperature": 25.6,
  "humidity": 61.2
}
```

## ESP32 firmware UART defaults

- UART port: `UART1`
- TX pin: `GPIO5`
- RX pin: `GPIO4`
- Baud rate: `115200`

If your hardware differs, update the constants in `main/app_main.c`.
