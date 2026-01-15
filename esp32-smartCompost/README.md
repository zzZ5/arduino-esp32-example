# ESP32 智能堆肥监测终端

基于 ESP32 + 多传感器 + MQTT 的堆肥过程实时监测与远程控制系统。

## 📖 简介

本项目专为堆肥过程监测设计，通过多种传感器实时采集堆肥过程中的关键环境参数（CO₂、O₂、温度、湿度等），实现智能化管理和远程控制，帮助优化堆肥工艺，提高堆肥效率。

### 核心功能
- 📊 **多传感器监测**：CO₂、O₂、堆体温度、空气温湿度
- 🎮 **远程控制**：曝气泵、抽气泵的远程开关控制
- ⏰ **定时任务**：支持定时执行控制指令
- 🔄 **设备管理**：远程重启、配置更新
- 💾 **数据持久化**：断电后保持采集节奏
- 🕐 **自动校时**：通过 NTP 自动校准时间

---

## 🛠 技术栈

| 类别 | 技术 |
|------|------|
| 硬件 | ESP32 |
| 框架 | Arduino Framework |
| 开发环境 | PlatformIO |
| 通信 | MQTT / WiFi |
| 时间同步 | NTP |
| 文件系统 | SPIFFS |
| 序列化 | ArduinoJson v7 |
| RTOS | FreeRTOS |

### 传感器模块
- **CO₂**：MH-Z16 (UART)
- **O₂**：DFRobot EOxygen (I2C)
- **温度**：DS18B20 (OneWire)
- **温湿度**：SHT31 (I2C)

---

## 📦 快速开始

### 硬件准备

- ESP32 开发板
- MH-Z16 CO₂ 传感器
- DFRobot O₂ 传感器
- DS18B20 温度传感器
- SHT31 温湿度传感器
- 5V 继电器 × 2
- 杜邦线、面包板

### 环境配置

```bash
# 1. 克隆项目
git clone <repository-url>
cd esp32-smartCompost

# 2. 配置 data/config.json
# 3. 上传配置文件
pio run --target uploadfs

# 4. 编译并上传
pio run --target upload
```

### 配置文件示例

```json
{
  "wifi": {
    "ssid": "你的WiFi名称",
    "password": "你的WiFi密码"
  },
  "mqtt": {
    "server": "MQTT服务器IP",
    "port": 1883,
    "user": "MQTT用户名",
    "pass": "MQTT密码",
    "device_code": "设备编号"
  },
  "ntp_servers": [
    "ntp.ntsc.ac.cn",
    "ntp.aliyun.com",
    "cn.ntp.org.cn"
  ],
  "pump_run_time": 60000,
  "read_interval": 60000
}
```

---

## 🔌 引脚接线

| 模块 | 引脚 | 说明 |
|------|------|------|
| 抽气泵 | GPIO 25 | 5V 继电器 |
| 曝气泵 | GPIO 26 | 5V 继电器 |
| MH-Z16 RX | GPIO 16 | UART 接收 |
| MH-Z16 TX | GPIO 17 | UART 发送 |
| DS18B20 | GPIO 4 | OneWire |
| O₂ / SHT31 | I2C | SDA/SCL |

---

## 📡 MQTT 通信

### Topic 结构

| 功能 | Topic |
|------|-------|
| 数据上传 | `compostlab/v2/{device_code}/telemetry` |
| 设备上线 | `compostlab/v2/{device_code}/register` |
| 远程控制 | `compostlab/v2/{device_code}/response` |

### 数据上传格式

```json
{
  "schema_version": 2,
  "ts": "2025-12-12 14:50:02",
  "channels": [
    { "code": "CO2", "value": 2.63, "unit": "%VOL", "quality": "OK" },
    { "code": "O2", "value": 14.78, "unit": "%VOL", "quality": "OK" },
    { "code": "RoomTemp", "value": 35.2, "unit": "℃", "quality": "OK" },
    { "code": "AirTemp", "value": 28.5, "unit": "℃", "quality": "OK" },
    { "code": "AirHumidity", "value": 62.1, "unit": "%RH", "quality": "OK" }
  ]
}
```

### 设备上线格式

```json
{
  "schema_version": 2,
  "ip_address": "192.168.1.100",
  "timestamp": "2025-12-12 14:50:02",
  "config": {
    "wifi": { "ssid": "沃土3", "password": "***" },
    "mqtt": {
      "server": "111.182.81.205",
      "port": 1883,
      "user": "equipment",
      "pass": "ZNXK8888",
      "device_code": "SmartCompost001"
    },
    "ntp_servers": ["ntp.ntsc.ac.cn", "ntp.aliyun.com"],
    "pump_run_time": 60000,
    "read_interval": 120000
  }
}
```

---

## 🎮 控制指令

### 基本格式

```json
{
  "device": "SmartCompost001",
  "commands": [...]
}
```

### 支持的命令

#### 1. 控制曝气泵

```json
{
  "command": "aeration",
  "action": "on|off",
  "duration": 5000
}
```

#### 2. 控制抽气泵

```json
{
  "command": "exhaust",
  "action": "on|off",
  "duration": 3000
}
```

#### 3. 定时任务

```json
{
  "command": "aeration",
  "action": "on",
  "duration": 10000,
  "schedule": "2025-12-15 18:00:00"
}
```

#### 4. 远程重启

```json
{ "command": "restart" }
```

#### 5. 更新配置

```json
{
  "command": "config_update",
  "config": {
    "pump_run_time": 80000,
    "read_interval": 120000
  }
}
```

---

## 📋 项目结构

```
esp32-smartCompost/
├── src/
│   ├── main.cpp              # 主程序
│   ├── config_manager.h/.cpp # 配置管理
│   ├── wifi_ntp_mqtt.h/.cpp # 网络/MQTT
│   └── sensor.h/.cpp         # 传感器驱动
├── data/
│   └── config.json           # 配置文件
├── platformio.ini            # PlatformIO 配置
└── README.md                 # 本文档
```

---

## ✨ 可靠性设计

- **断电保持**：NVS 记录采集时间，断电后恢复节奏
- **异常处理**：传感器失败自动返回 -1，避免 NaN
- **数据质量**：每个数据附带质量标识（OK/ERR）
- **自动重试**：网络连接失败自动重试
- **看门狗防护**：长时间操作分段 delay + yield

---

## 🤝 贡献指南

欢迎任何形式的贡献！

### 提交规范

```
<类型>: <简短描述>

<详细说明>
```

类型：`feat` | `fix` | `docs` | `style` | `refactor` | `test` | `chore`

### 开发流程

1. Fork 项目
2. 创建分支 `feature/xxx` 或 `fix/xxx`
3. 提交修改
4. 推送到分支
5. 创建 Pull Request

---

## 📄 许可证

MIT License - 详见 [LICENSE](LICENSE)

---

## 📞 联系方式

- **Issues**：[GitHub Issues](../../issues)
- **讨论**：[GitHub Discussions](../../discussions)

---

## 📚 相关资源

- [PlatformIO 文档](https://docs.platformio.org/)
- [ArduinoJson 文档](https://arduinojson.org/)
- [ESP32 数据手册](https://www.espressif.com/sites/default/files/documentation/esp32_datasheet_en.pdf)
- [MQTT 协议规范](http://mqtt.org/)

---

## 🙏 致谢

感谢以下开源项目：

- ArduinoJson
- PubSubClient
- DFRobot EOxygen Sensor
- Adafruit SHT31 Library

---

**项目状态**：🟢 正在维护中  
最后更新：2025-01-15
