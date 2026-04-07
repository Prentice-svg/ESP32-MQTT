# ESP32-C3 直连 OneNet 传感器节点

这是一个独立的 PlatformIO 工程，目标是让 ESP32-C3 直接完成：

- 读取 AHT30 温湿度
- 读取 HW-390 类土壤湿度传感器
- 通过 Wi-Fi 连接 OneNet MQTT
- 直接上报属性到 OneNet 云端

你可以直接用 VS Code + PlatformIO 打开这个目录：

`E:\Dev\esp32-mqtt\pio-onenet-sensors`

## 当前已配置好的内容

- 开发框架：PlatformIO + Arduino
- 目标芯片：ESP32-C3
- 串口上传端口：`COM3`
- 串口监视器端口：`COM3`
- 串口监视器波特率：`115200`
- OneNet 温度标识符：`EnvironmentTemperature`
- OneNet 湿度标识符：`EnvironmentHumidity`
- OneNet 土壤湿度标识符：`SoilMoisture`

## 你需要先修改的文件

打开：

`include/app_config.h`

把下面这些内容改成你自己的：

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `ONENET_PRODUCT_ID`
- `ONENET_DEVICE_NAME`
- `ONENET_DEVICE_TOKEN`
- `PROP_SOIL_MOISTURE`
- `I2C_SDA_PIN`
- `I2C_SCL_PIN`
- `SOIL_SENSOR_PIN`
- `SOIL_ADC_DRY`
- `SOIL_ADC_WET`

## 传感器接线说明

### 1. AHT30 接线

AHT30 是 I2C 传感器，通常有 4 根线：

- `VCC`
- `GND`
- `SDA`
- `SCL`

默认程序里使用的是：

- `SDA -> GPIO8`
- `SCL -> GPIO9`

推荐接法：

- `AHT30 VCC -> ESP32-C3 3V3`
- `AHT30 GND -> ESP32-C3 GND`
- `AHT30 SDA -> ESP32-C3 GPIO8`
- `AHT30 SCL -> ESP32-C3 GPIO9`

注意：

- AHT30 一般使用 `3.3V`
- 如果你的模块板上带了上拉电阻，通常可以直接接
- 如果你实际接到了别的引脚，就改 `app_config.h` 里的 `I2C_SDA_PIN` 和 `I2C_SCL_PIN`

### 2. HW-390 土壤湿度传感器接线

这类土壤湿度模块常见有两个输出：

- `AO`：模拟输出
- `DO`：数字输出

当前程序用的是模拟量采集，所以你应该接 `AO`，不是 `DO`。

推荐接法：

- `HW-390 VCC -> ESP32-C3 3V3`
- `HW-390 GND -> ESP32-C3 GND`
- `HW-390 AO -> ESP32-C3 GPIO4`

默认程序里使用的是：

- `SOIL_SENSOR_PIN = GPIO4`

注意：

- 建议整个链路都用 `3.3V`
- 如果你的 HW-390 模块在 `5V` 下输出模拟电压，可能会超过 ESP32-C3 ADC 输入安全范围，不建议直接这样接
- 如果你接的是数字输出 `DO`，那就不是当前这份代码的采样方式了，需要改程序

## OneNet 属性上报格式

程序会发这种 property post：

```json
{
  "id": "12345",
  "version": "1.0",
  "params": {
    "EnvironmentTemperature": { "value": 25.6 },
    "EnvironmentHumidity": { "value": 61.2 },
    "SoilMoisture": { "value": 48 }
  }
}
```

这里有个关键点：

- `temperature` 必须和你 OneNet 物模型里的温度标识符完全一致
- `humidity` 必须和你 OneNet 物模型里的湿度标识符完全一致
- `soil_moisture` 也必须和你 OneNet 物模型里的土壤湿度标识符完全一致

目前我已经按你的 OneNet 物模型配置好了：

- `EnvironmentTemperature`
- `EnvironmentHumidity`
- `SoilMoisture`

## 土壤湿度校准说明

土壤湿度模拟传感器不同模块差异很大，所以建议你自己做一次干湿校准。

程序里现在有两个参数：

```cpp
#define SOIL_ADC_DRY 3000
#define SOIL_ADC_WET 1200
```

你可以这样校准：

1. 先把探头完全放在空气中，记下串口看到的 ADC 值，作为 `SOIL_ADC_DRY`
2. 再把探头插入湿润土壤或接近水的状态，记下 ADC 值，作为 `SOIL_ADC_WET`
3. 把这两个值填回 `app_config.h`

程序会把 ADC 值映射成 `0~100` 的百分比。

## 编译、烧录、串口监视

在这个目录下直接执行：

```bash
pio run
pio run -t upload
pio device monitor
```

如果你的板子不是 `COM3`，改：

`platformio.ini`

里面的：

- `upload_port`
- `monitor_port`

## 默认引脚汇总

- AHT30 SDA：`GPIO8`
- AHT30 SCL：`GPIO9`
- 土壤湿度模拟输入：`GPIO4`

## 额外提醒

- `esp32-c3-devkitm-1` 是一个比较通用的 ESP32-C3 板型配置，很多 ESP32-C3 Super Mini 都能用
- 如果你后面发现某块板子在上传或 USB 识别上表现不稳定，可以再换 PlatformIO 的 board 定义
- 现在这份工程已经能本地编译通过
