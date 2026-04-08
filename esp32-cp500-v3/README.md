# ESP32 CP500 控制器

基于 ESP32 的温控与执行器控制项目，支持多路 DS18B20 温度采集、加热器控制、水泵控制、PWM 曝气、MQTT 遥测/命令、SPIFFS 配置持久化，以及急停与多层安全保护。

## 文档索引

- [MQTT 协议文档](./docs/MQTT_PROTOCOL.md)
- [项目概览](#项目概览)
- [硬件接线](#硬件接线)
- [配置说明](#配置说明)
- [MQTT 与远程控制](#mqtt-与远程控制)
- [控制逻辑](#控制逻辑)
- [开发与构建](#开发与构建)

## 项目概览

当前固件的核心能力：

- 内部总线采集 2 路温度：核心温度 `TempIn`、水箱温度 `TankTemp`
- 外部总线采集最多 3 路温度：`TempOut1` 到 `TempOut3`
- 支持两种自动控制模式
- `n-curve` 自适应补热
- `bath_setpoint` 定点恒温
- 支持加热器与水泵联动控制
- 支持 PWM 曝气与定时曝气
- 支持 MQTT 遥测、上线消息、命令下发、远程配置更新
- 支持急停、外浴温度上限保护、水箱温度保护、最小启停时间保护
- 支持 NVS 保存测量/曝气节拍相位，重启后继续运行

项目基于 PlatformIO，目标板卡为 `esp32dev`，文件系统使用 `SPIFFS`。

## 硬件接线

当前代码中的引脚定义来自 [main.cpp](./src/main.cpp#L1183)：

| 功能 | GPIO | 说明 |
|-----|------|------|
| 内部温度总线 | 4 | DS18B20，index 0 为核心温度，index 1 为水箱温度 |
| 外部温度总线 | 5 | DS18B20，最多取前 3 个探头 |
| 加热器输出 | 25 | 数字输出 |
| 水泵输出 | 26 | 数字输出 |
| 曝气输出 | 27 | LEDC PWM 输出 |

传感器与执行器初始化位于 [sensor.cpp](./src/sensor.cpp)。

## 软件架构

主要模块如下：

| 文件 | 作用 |
|-----|------|
| [src/main.cpp](./src/main.cpp) | 主控制逻辑、MQTT 回调、命令调度、启动流程 |
| [src/config_manager.cpp](./src/config_manager.cpp) | 配置加载/保存、默认值、Topic 构建 |
| [src/sensor.cpp](./src/sensor.cpp) | 传感器采集、执行器控制、曝气 PWM |
| [src/wifi_ntp_mqtt.cpp](./src/wifi_ntp_mqtt.cpp) | WiFi、NTP、MQTT 连接与发布 |
| [src/emergency_stop.cpp](./src/emergency_stop.cpp) | 急停状态机 |
| [data/config.json](./data/config.json) | 默认配置文件样例 |
| [docs/MQTT_PROTOCOL.md](./docs/MQTT_PROTOCOL.md) | 独立 MQTT 协议文档 |

FreeRTOS 任务模型：

- `measurementTask`：周期测量、自动控制、遥测发布
- `commandTask`：检查并执行待处理命令队列
- `loop()`：维持 MQTT 连接

## 开发与构建

### 环境要求

- VS Code + PlatformIO
- ESP32 Arduino Framework
- Python 环境由 PlatformIO 自动管理

### 依赖库

依赖配置见 [platformio.ini](./platformio.ini)：

- `bblanchon/ArduinoJson@^7.4.2`
- `knolleary/PubSubClient@^2.8`
- `milesburton/DallasTemperature@^4.0.4`

### 常用命令

```bash
pio run
pio run --target upload
pio run --target uploadfs
pio device monitor
```

含义如下：

- `pio run`：编译固件
- `pio run --target upload`：烧录固件
- `pio run --target uploadfs`：上传 `data/` 目录到 SPIFFS
- `pio device monitor`：查看串口日志

## 启动流程

系统启动流程大致如下：

1. 挂载 SPIFFS
2. 读取 `/config.json`
3. 若配置缺失或解析失败，则填充默认值并继续启动
4. 初始化传感器、执行器、急停模块
5. 连接 WiFi
6. 初始化 NTP
7. 连接 MQTT
8. 发布上线消息到 `register` Topic
9. 恢复上次测量与曝气相位
10. 启动后台任务

当前策略说明：

- 只有 SPIFFS 挂载失败才视为严重故障
- 配置文件缺失或损坏时，不会直接重启，而是使用默认值继续运行
- MQTT 连接失败时，系统仍可继续本地控制

## 配置说明

配置文件路径：

- `/config.json`
- 默认样例见 [data/config.json](./data/config.json)

### 配置结构示例

```json
{
  "wifi": {
    "ssid": "Compostlab",
    "password": "znxk8888"
  },
  "mqtt": {
    "server": "118.25.108.254",
    "port": 1883,
    "user": "equipment",
    "pass": "ZNXK8888",
    "device_code": "CP500-VT001"
  },
  "post_interval": 60000,
  "ntp_host": [
    "ntp.ntsc.ac.cn",
    "ntp.aliyun.com",
    "cn.ntp.org.cn"
  ],
  "temp_limitout_max": 65,
  "temp_limitin_max": 70,
  "temp_limitout_min": 25,
  "temp_limitin_min": 25,
  "temp_maxdif": 13,
  "bath_setpoint": {
    "enabled": false,
    "target": 65.0,
    "hyst": 0.8
  },
  "aeration_timer": {
    "enabled": true,
    "interval": 300000,
    "duration": 300000
  },
  "safety": {
    "tank_temp_max": 90.0
  },
  "heater_guard": {
    "min_on_ms": 30000,
    "min_off_ms": 30000
  },
  "pump_adaptive": {
    "delta_on_min": 6.0,
    "delta_on_max": 25.0,
    "hyst_nom": 3.0,
    "ncurve_gamma": 1.3
  },
  "pump_learning": {
    "step_up": 0.5,
    "step_down": 0.2,
    "max": 10.0,
    "progress_min": 0.05
  },
  "curves": {
    "in_diff_ncurve_gamma": 2.0
  }
}
```

### 关键配置项

| 路径 | 类型 | 说明 |
|-----|------|------|
| `wifi.ssid` | String | WiFi 名称 |
| `wifi.password` | String | WiFi 密码 |
| `mqtt.server` | String | MQTT 服务器地址 |
| `mqtt.port` | Number | MQTT 端口 |
| `mqtt.user` | String | MQTT 用户名 |
| `mqtt.pass` | String | MQTT 密码 |
| `mqtt.device_code` | String | 设备编码，用于生成 Topic |
| `ntp_host` | Array | NTP 服务器列表 |
| `post_interval` | Number | 测量与上报周期，单位 ms |
| `temp_limitout_max` | Number | 外浴温度上限保护 |
| `temp_limitout_min` | Number | 外浴温度下限参考值 |
| `temp_limitin_max` | Number | 内部温度上限参考值 |
| `temp_limitin_min` | Number | 内部温度下限参考值 |
| `temp_maxdif` | Number | 温差阈值上限，n-curve 相关 |
| `bath_setpoint.enabled` | Bool | 是否启用定点恒温模式 |
| `bath_setpoint.target` | Number | 目标外浴温度 |
| `bath_setpoint.hyst` | Number | 回差 |
| `aeration_timer.enabled` | Bool | 是否启用定时曝气 |
| `aeration_timer.interval` | Number | 曝气间隔，单位 ms |
| `aeration_timer.duration` | Number | 每次曝气持续时间，单位 ms |
| `safety.tank_temp_max` | Number | 水箱温度上限 |
| `heater_guard.min_on_ms` | Number | 加热器最短开机时间 |
| `heater_guard.min_off_ms` | Number | 加热器最短关机时间 |
| `pump_adaptive.delta_on_min` | Number | 水泵联动阈值下限 |
| `pump_adaptive.delta_on_max` | Number | 水泵联动阈值上限 |
| `pump_adaptive.hyst_nom` | Number | 名义回差 |
| `pump_adaptive.ncurve_gamma` | Number | 水泵阈值曲线指数 |
| `pump_learning.step_up` | Number | 学习上调步长 |
| `pump_learning.step_down` | Number | 学习下调步长 |
| `pump_learning.max` | Number | 学习补偿上限 |
| `pump_learning.progress_min` | Number | 仅水泵升温有效判定阈值 |
| `curves.in_diff_ncurve_gamma` | Number | `t_in` 差值曲线指数 |

### 默认值与兜底

默认值定义见 [config_manager.cpp](./src/config_manager.cpp)。

当前主要默认值包括：

- `post_interval = 60000`
- `temp_maxdif = 5`
- `temp_limitout_max = 75`
- `temp_limitin_max = 70`
- `temp_limitout_min = 25`
- `temp_limitin_min = 25`
- `aeration_interval = 600000`
- `aeration_duration = 300000`
- `tank_temp_max = 90.0`
- `heater_min_on_ms = 30000`
- `heater_min_off_ms = 30000`
- `pump_delta_on_min = 6.0`
- `pump_delta_on_max = 25.0`
- `pump_hyst_nom = 3.0`
- `pump_ncurve_gamma = 1.3`
- `pump_learning.max = 8.0`
- `bath_setpoint.enabled = false`
- `bath_setpoint.target = 45.0`
- `bath_setpoint.hyst = 0.8`

## MQTT 与远程控制

Topic 由 [config_manager.cpp](./src/config_manager.cpp) 自动拼接：

- 遥测：`compostlab/v2/{device_code}/telemetry`
- 命令：`compostlab/v2/{device_code}/response`
- 上线：`compostlab/v2/{device_code}/register`

更完整的报文格式请看 [MQTT_PROTOCOL.md](./docs/MQTT_PROTOCOL.md#L1)。

### 遥测上报

遥测报文采用 `schema_version = 2`，由 `channels` 数组组成，典型通道包括：

- `TempIn`
- `TempOut1`
- `TempOut2`
- `TempOut3`
- `TankTemp`
- `Heater`
- `Pump`
- `Aeration`
- `EmergencyState`

### 上线消息

设备启动完成后会向 `register` Topic 发送上线消息，包含：

- 当前 IP
- 当前配置快照
- NTP 服务器列表
- 主要控制参数

安全说明：

- WiFi 密码不会明文上报
- MQTT 密码不会明文上报
- 当前固件会用 `********` 掩码替代

### 手动控制命令

支持的手动命令：

- `heater`
- `pump`
- `aeration`
- `fan`

其中 `fan` 在设备内部会映射成 `aeration`。

#### `action` 语义

| action | 说明 |
|-----|------|
| `on` | 打开设备，并进入手动锁 |
| `off` | 关闭设备，并清除该设备的手动锁与待执行定时命令 |
| `auto` | 解除手动锁，让自动控制重新接管 |

#### `duration` 语义

| duration | 说明 |
|-----|------|
| `> 0` | 临时手动锁，超时后自动补发 `off` |
| `= 0` | 持续手动锁，不自动释放 |
| 未提供 | 等同于 `0` |

#### 关键规则

- 同一设备收到新的手动命令时，会清掉旧的待执行定时 `off`
- `auto` 不会强制开/关设备，只会释放手动锁
- `heater on` 仍受 Tank 安全保护约束
- 急停状态下，普通手动命令会被拒绝

#### 示例

临时手动开启加热器 5 分钟：

```json
{
  "commands": [
    {
      "command": "heater",
      "action": "on",
      "duration": 300000
    }
  ]
}
```

持续手动开启水泵：

```json
{
  "commands": [
    {
      "command": "pump",
      "action": "on",
      "duration": 0
    }
  ]
}
```

解除曝气手动锁：

```json
{
  "commands": [
    {
      "command": "aeration",
      "action": "auto"
    }
  ]
}
```

### 急停命令

激活急停：

```json
{
  "commands": [
    {
      "command": "emergency",
      "action": "on"
    }
  ]
}
```

解除急停：

```json
{
  "commands": [
    {
      "command": "emergency",
      "action": "off"
    }
  ]
}
```

### 远程配置更新

设备支持 `config_update` 命令。处理逻辑位于 [main.cpp](./src/main.cpp#L430) 附近。

示例：

```json
{
  "commands": [
    {
      "command": "config_update",
      "config": {
        "post_interval": 60000,
        "bath_setpoint": {
          "enabled": true,
          "target": 65.0,
          "hyst": 0.8
        }
      }
    }
  ]
}
```

行为说明：

- 设备会更新内存中的配置
- 保存到 `/config.json`
- 保存成功后自动重启

## 控制逻辑

### 控制优先级

系统控制优先级从高到低如下：

1. 急停
2. 安全保护
3. 自动控制逻辑
4. 手动锁约束
5. 执行层最小启停时间与定时任务

### 自动控制模式

#### 1. n-curve 模式

当 `bath_setpoint.enabled = false` 时启用。

核心思路：

- 根据 `t_in` 与外浴中位温 `t_out_med` 的差值判断是否需要补热
- 根据 `t_in` 的相对区间动态计算阈值
- 结合 `t_tank - t_out_med` 决定是否让水泵参与联动
- 学习机制会根据仅水泵阶段是否有效升温，动态调节阈值

适合：

- 对温差变化更敏感的自适应场景
- 需要根据热量传递效率自动调节泵介入时机的场景

#### 2. setpoint 模式

当 `bath_setpoint.enabled = true` 时启用。

核心思路：

- 根据外浴中位温与 `target`、`hyst` 比较
- 低于下界时补热
- 高于上界时停止补热
- 落在回差带内时保持当前策略稳定

适合：

- 目标温度明确的恒温场景
- 联调阶段希望行为更可预测的场景

### 安全机制

#### 水箱温度保护

- Tank 温度无效时，不允许自动加热
- Tank 温度过高时，强制关闭加热器
- 手动 `heater on` 也会执行同样的安全检查

#### 外浴温度上限保护

- 当外浴中位温超过 `temp_limitout_max` 时，强制停热
- 同时关闭水泵
- 并清理相关手动锁

#### 加热器最小启停时间保护

- `heater_guard.min_on_ms`
- `heater_guard.min_off_ms`

用于避免继电器频繁抖动。

#### 急停

急停模块见 [emergency_stop.cpp](./src/emergency_stop.cpp)。

激活后行为：

- 立即关闭加热器
- 立即关闭水泵
- 立即关闭曝气
- 阻断自动控制
- 阻断普通手动命令
- 测量与数据上报仍继续

### 手动锁与定时关断

实现位于 [main.cpp](./src/main.cpp#L470) 附近。

当前语义：

- `on + duration > 0`：临时手动锁
- `on + duration = 0`：持续手动锁
- `off`：关闭设备并清除相关待执行命令
- `auto`：释放手动锁，恢复自动控制

调度机制：

- 命令队列使用 `millis()` 做毫秒级调度
- 不依赖 NTP 当前时间
- 对同一设备会清理旧的延时关断命令，避免残留误触发

### 曝气控制

曝气由 [sensor.cpp](./src/sensor.cpp) 中的 LEDC PWM 控制实现。

支持：

- 定时曝气
- 软启动
- 软停止
- 最大占空比限制
- 可选 kick 启动参数

当前项目默认已经具备软启动控制接口，但是否启用取决于后续参数配置与调用策略。

## 网络行为

### WiFi

- 启动时尝试连接 WiFi
- MQTT 重连时如果 WiFi 断开，会先重连 WiFi

### NTP

- 支持多个 NTP 服务器
- 配置项为 `ntp_host`

### MQTT

- 保持长连接
- 断线时自动重连
- 若长时间没有成功发布，会触发重连

当前固件不再依赖公网 DNS 或公网 IP 探测来判定网络是否“可用”，因此更适合局域网 MQTT 或离线控制环境。

## 数据持久化

项目中同时使用两种持久化方式：

- `SPIFFS`：保存配置文件 `/config.json`
- `NVS`：保存最近测量时间与最近曝气时间

NVS 的作用：

- 设备重启后恢复测量相位
- 设备重启后恢复曝气节拍

## 常见联调建议

- 初次联调先确认 `mqtt.device_code` 唯一
- 先用 `response` Topic 单独测试 `heater`、`pump`、`aeration`
- 手动调试完成后，使用 `action = "auto"` 释放手动锁
- 联调 setpoint 模式时，优先从较小回差开始观察
- 如果需要平台接入，优先阅读 [MQTT_PROTOCOL.md](./docs/MQTT_PROTOCOL.md#L1)

## 已知实现特性

- 遥测使用 `channels` 数组，不是固定字段平铺结构
- 内部温度总线会一次转换、批量读取，避免重复转换带来的阻塞
- 启动上报中的密码字段已做掩码处理
- 配置文件缺失时会使用默认值继续启动
- `fan` 命令是 `aeration` 的兼容别名

## 后续建议

如果后面继续维护这个项目，推荐的工程化方向是：

- 继续拆分 [main.cpp](./src/main.cpp)
- 统一源码注释编码，全部转为 UTF-8
- 为关键控制逻辑补测试或仿真用例
- 继续压缩动态 `String` 与 JSON 临时对象的使用

## 版本说明

本文档基于当前仓库代码状态整理，和历史 README 的旧版说明相比，优先以源码实现为准。
