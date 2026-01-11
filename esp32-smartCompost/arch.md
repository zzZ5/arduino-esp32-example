# 📘 ESP32 堆肥监测终端

该系统基于 **ESP32 + 多传感器 + MQTT**，用于堆肥过程的实时监测与远程控制。设备可采集 CO₂、O₂、堆体温度、空气温湿度、含水率，并通过 MQTT 上传，同时支持远程控制曝气、抽气泵以及设备远程重启。

---

## 1. 设备主要功能（简要）

* **传感器监测**

  * MH-Z16（CO₂）
  * DFRobot O₂
  * DS18B20 堆体温度
  * FDS100 含水率
  * DHT22（空气温湿度）
* **执行机构**

  * 抽气泵（采样用）
  * 曝气泵（供氧/调节用）
* **网络能力**

  * WiFi 自动连接
  * NTP 自动校时
  * MQTT 双向通信
* **远程操作**

  * 开/关曝气
  * 开/关抽气
  * 定时执行任务（schedule）
  * **远程重启设备**
* **可靠性**

  * 断电后保持采集节奏
  * 失败自动重试
  * 无 NaN 上传（异常自动置 -1）

---

# 2. 数据上传机制（详细）

设备定期采集所有传感器数据，并将数据打包成 JSON，然后通过 MQTT 的：

```
post_topic
```

上传到服务器。

上传格式如下（系统实际上传形式）：

```json
{
  "data": [
    { "value": 2.63, "key": "CO2_key", "measured_time": "2025-12-12 14:50:02" },
    { "value": 14.78, "key": "O2_key",  "measured_time": "2025-12-12 14:50:02" },
    { "value": 7.9, "key": "RoomTemp_key", "measured_time": "2025-12-12 14:50:02" },
    { "value": 45.3, "key": "Mois_key", "measured_time": "2025-12-12 14:50:02" },
    { "value": 23.4, "key": "AirTemp_key", "measured_time": "2025-12-12 14:50:02" },
    { "value": 62.1, "key": "AirHumidity_key", "measured_time": "2025-12-12 14:50:02" }
  ]
}
```

说明：

| 字段            | 说明                         |
| ------------- | -------------------------- |
| value         | 传感器值（自动处理 NaN，不会上传无效 JSON） |
| key           | 每种传感器在配置文件中的 key（可配置）      |
| measured_time | ESP32 NTP 校准后的时间戳          |

**特点：**

* **CO₂ 采用多次测量取平均，自动过滤错误读数**
* **DHT22 采用 DHTesp，读数稳定**
* **所有 key 来自 config.json，可动态替换**
* **不会出现 NaN，系统自动改为 -1**

---

# 3. MQTT 远程控制机制（详细）

ESP32 监听：

```
response_topic
```

服务器向该 topic 发送控制命令，格式如下：

```json
{
  "device": "设备编号",
  "commands": [
    { ... }
  ]
}
```

ESP32 会检查 `"device"` 字段是否与本机匹配，防止误控制。

---

## 3.1 控制曝气泵（aeration）

### 开启（可带时长）

```json
{
  "device": "xxxx",
  "commands": [
    { "command": "aeration", "action": "on", "duration": 5000 }
  ]
}
```

说明：

* `"action": "on"` 开启曝气泵（GPIO26 = HIGH）
* `"duration": 5000` 表示 5 秒后自动关闭
* 若不带 duration，则手动控制

### 关闭

```json
{
  "command": "aeration",
  "action": "off"
}
```

---

## 3.2 控制抽气泵（exhaust）

```json
{
  "command": "exhaust",
  "action": "on",
  "duration": 3000
}
```

抽气泵用于采样，通常由系统自动控制，但也支持远程手动触发。

---

## 3.3 定时控制（schedule）

可对任意控制指令加 schedule：

```json
{
  "command": "aeration",
  "action": "on",
  "duration": 5000,
  "schedule": "2025-12-15 18:00:00"
}
```

ESP32 会将该任务加入队列，到时自动执行。

---

# 4. 远程重启功能（详细）

这是运维必备功能，可以从后台让设备强制重启。

### 指令格式：

```json
{
  "device": "xxxx",
  "commands": [
    { "command": "reboot" }
  ]
}
```

### 设备行为：

1. 立即打印日志：

   ```
   [MQTT] 收到远程重启命令
   ```
2. 延时 500ms
3. 执行：

```cpp
ESP.restart();
```

> ⚠️ 该功能立即执行，不进入任务队列，保证运维可靠性。

---

# 5. 本地持久化功能（NVS）

系统通过 ESP32 的 NVS 保留：

* **上次成功采集时间**

用于断电重启后恢复采集节奏，不会导致上传时间错乱或采集过快。

---

# 6. 任务架构（FreeRTOS）

系统包含两个后台任务（独立于 loop）：

| 任务              | 功能                   |
| --------------- | -------------------- |
| measurementTask | 定时执行传感器测量与上传         |
| commandTask     | 执行定时指令（schedule）与泵控制 |

主循环只负责 MQTT 心跳：

```cpp
void loop() {
  maintainMQTT(5000);
  delay(100);
}
```

---

# 📌 总结

设备支持：

### ✔ 完整的数据监测与上传（6 种传感器）

### ✔ MQTT 远程控制（开关曝气、抽气、定时任务）

### ✔ MQTT 远程重启

### ✔ 自动校时、自动联网

### ✔ 稳定的传感器读取（支持异常保护）

### ✔ 配置文件可动态管理 key 和周期