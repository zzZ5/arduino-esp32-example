# ESP32 智能温控与远程控制系统

本项目基于 ESP32 实现堆肥反应器的温度监测、加热控制与远程命令调度，支持 MQTT 数据上传和日志记录功能。系统包括：

* 多点温度采集与中位数计算
* 自动判断是否加热（含温差逻辑）
* 加热器与曝气控制（本地或远程触发）
* MQTT 数据发布与控制命令接收
* SPIFFS 日志系统（带最大容量限制）
* 配置通过 JSON 文件灵活管理

---

## 📁 项目结构

```
.
├── src/
│   ├── main.cpp               // 主控制逻辑与任务调度
│   ├── config_manager.*       // 读取配置文件 /config.json
│   ├── wifi_ntp_mqtt.*        // 网络连接、NTP 同步与 MQTT 管理
│   ├── sensor.*               // 传感器初始化与读取函数
│   ├── log_manager.*          // 日志系统，写入 SPIFFS /log.txt
│
├── data/
│   └── config.json            // 配置文件，上传至 SPIFFS
│
├── platformio.ini             // 编译配置（适用于 PlatformIO）
└── README.md                  // 项目说明文档（本文件）
```

---

## ⚙️ 功能说明

### 📡 远程控制（通过 MQTT）
  
通过 MQTT topic（如 `response_topic`）接收如下格式控制指令：

```json
{
  "device": "your_equipment_key",
  "commands": [
    {
      "command": "heater",
      "action": "on",
      "duration": 5000,
      "schedule": "2025-07-29 14:00:00"
    },
    {
      "command": "aeration",
      "action": "off"
    }
  ]
}
```

* `command` 可为 `"heater"` 或 `"aeration"`
* `action` 为 `"on"` 或 `"off"`
* `duration`（可选）为持续时间（毫秒）
* `schedule`（可选）为延迟执行时间（UTC）

### 🎛 配置上传（通过 MQTT）

通过 MQTT topic（如 `response_topic`）接收如下格式配置，会更新当前配置文件：

```json
{
  "device": "your_equipment_key",
  "commands": [
    {
      "command": "config_update",
      "config": {
        "temp_maxdif": 5
        }
    }
  ]
}
```

* `command` 为 `"config_update"`
* `config` 中参数可选，具体可配置的参数见后续说明。配置修改后自动重启生效

### 🌡️ 温度采集与加热控制逻辑

系统每隔一定时间自动采集：

* 内部温度（单点）
* 外部温度（多个探头）

控制逻辑如下：

* 若外部或内部温度超过上下限，立即关断/开启加热器
* 若温度处于合理区间，通过“内部温度 - 外部中位数”的差值判断是否开启加热
* 控制状态可上传并记录

### 🧠 任务调度

系统中创建两个任务：

* `measurementTask`：定时采集温度、执行加热控制并通过 MQTT 上传数据
* `commandTask`：定时检查是否有待执行的控制命令（支持定时调度）

---

## 📦 配置文件（/config.json）

配置文件通过 SPIFFS 上传，示例格式如下：

```json
{
  "wifi": {
    "ssid": "your_wifi_ssid",
    "password": "your_wifi_password"
  },
  "mqtt": {
    "server": "broker_address",
    "port": 1883,
    "user": "mqtt_user",
    "password": "mqtt_pass",
    "client_id": "your_device_id",
    "post_topic": "your/post/topic",
    "response_topic": "your/response/topic"
  },
  "ntp_servers": [
    "ntp.aliyun.com",
    "cn.ntp.org.cn",
    "ntp.tuna.tsinghua.edu.cn"
  ],
  "equipment_key": "your_equipment_key",
  "keyTempIn": "T_in",
  "keyTempOut": ["T1", "T2", "T3"],
  "tempLimitOutMax": 40.0,
  "tempLimitOutMin": 10.0,
  "tempLimitInMax": 65.0,
  "tempLimitInMin": 30.0,
  "tempMaxDiff": 10.0,
  "postInterval": 600000
}
```

---

## 🪵 日志记录（log.txt）

系统日志写入 SPIFFS 根目录下 `/log.txt`，格式如下：

```
[2025-07-29 10:00:01] [INFO] 加热器开启，当前温差过大
[2025-07-29 10:10:01] [WARN] 收到未知命令：fan
[2025-07-29 10:15:01] [ERROR] 数据上传失败
```

* 可配置最大日志文件大小（默认 50KB）
* 超出后自动清空日志
* 支持读取全部日志 `readAllLogs()` 调试使用

---

## 🚀 使用说明

1. 修改并上传 `/data/config.json` 至 SPIFFS（可用 PlatformIO 的 Upload File System Image）
2. 编译并烧录 ESP32 程序
3. 上电后设备将自动：

   * 连接 WiFi
   * 同步 NTP 时间
   * 连接 MQTT 服务器
   * 启动温度采集任务与命令调度任务

---

## 📌 依赖库（PlatformIO）

```ini
lib_deps =
  bblanchon/ArduinoJson@^6.21.3
  knolleary/PubSubClient
  me-no-dev/ESP Async WebServer
  me-no-dev/AsyncTCP
```

---

## 📞 联系方式

如需交流该系统的堆肥控制应用、自动化部署或数据平台集成，欢迎联系作者。

---

