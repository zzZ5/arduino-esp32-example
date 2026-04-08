# esp32-MMCGS

ESP32 多点气体检测控制程序。

本项目用于 6 个采样点的轮询气体检测。当前版本针对“样气有限”的现场场景，采用“短时取样抽气 + 停泵静态检测”的策略，目标是在尽量少耗气的前提下完成检测与上报。

## 项目概览

### 传感器

- `MH-Z16`
  读取 `CO2`
- `ZCE04B`
  读取 `CO / H2S / O2 / CH4`
- `SHT30`
  读取空气温度和湿度

### 泵路

- `point1 ~ point6`
  六个采样点泵
- `purge`
  一路吹扫泵

### 当前引脚

- `MH-Z16`
  - RX: `18`
  - TX: `19`
- `ZCE04B`
  - RX: `16`
  - TX: `17`
- `SHT30`
  - SDA: `21`
  - SCL: `22`
- Pumps
  - `P1=13`
  - `P2=14`
  - `P3=25`
  - `P4=26`
  - `P5=27`
  - `P6=32`
  - `Purge=33`

## 代码结构

- [src/main.cpp](/d:/ArduinoProject/arduino-esp32-example/esp32-MMCGS/src/main.cpp)
  启动流程、测量调度、MQTT 回调、判稳、NVS 恢复
- [src/sensor.cpp](/d:/ArduinoProject/arduino-esp32-example/esp32-MMCGS/src/sensor.cpp)
  传感器驱动与泵控制
- [src/config_manager.h](/d:/ArduinoProject/arduino-esp32-example/esp32-MMCGS/src/config_manager.h)
  配置结构定义
- [src/config_manager.cpp](/d:/ArduinoProject/arduino-esp32-example/esp32-MMCGS/src/config_manager.cpp)
  SPIFFS 配置读写
- [src/wifi_ntp_mqtt.cpp](/d:/ArduinoProject/arduino-esp32-example/esp32-MMCGS/src/wifi_ntp_mqtt.cpp)
  WiFi、NTP、MQTT、上报、补传
- [src/data_buffer.cpp](/d:/ArduinoProject/arduino-esp32-example/esp32-MMCGS/src/data_buffer.cpp)
  离线缓存

## 启动流程

设备上电后流程如下：

1. 初始化串口和命令队列互斥锁
2. 挂载 SPIFFS，读取 `/config.json`
3. 初始化离线缓存模块
4. 连接 WiFi
5. 同步 NTP 时间
6. 连接 MQTT
7. 订阅控制响应主题
8. 初始化传感器和泵引脚
9. 预热并读取一次 `MH-Z16`
10. 发布上线注册消息
11. 尝试补传历史缓存
12. 从 NVS 恢复上次巡检状态
13. 启动测量任务和命令任务

## 配置项

当前核心配置如下：

```json
{
  "sample_time": 15000,
  "static_measure_time": 30000,
  "purge_pump_time": 30000,
  "read_interval": 1200000
}
```

### 字段说明

- `sample_time`
  单个点位的取样抽气时间，单位毫秒
- `static_measure_time`
  停泵后静态检测窗口时间，单位毫秒
- `purge_pump_time`
  点位结束后吹扫气路时间，单位毫秒
- `read_interval`
  整轮巡检启动周期，单位毫秒

### 配置文件位置

- [data/config.json](/d:/ArduinoProject/arduino-esp32-example/esp32-MMCGS/data/config.json)

### 其它配置

`config.json` 中还包含：

- `wifi.ssid`
- `wifi.password`
- `mqtt.server`
- `mqtt.port`
- `mqtt.user`
- `mqtt.pass`
- `mqtt.device_code`
- `mqtt.point_device_codes`
- `ntp_servers`

## 检测流程

### 整轮巡检

- 系统按 `read_interval` 启动一轮巡检
- 一轮中依次处理 `point1 ~ point6`
- 每个点位结束后执行一次吹扫
- 如果本轮耗时已经超过 `read_interval`，下一轮会立即开始

### 单点检测

每个点位包含两阶段：

1. 取样抽气阶段
   - 打开当前点位泵
   - 持续 `sample_time`
   - 目的：把有限样气送入传感器腔体
2. 静态检测阶段
   - 关闭当前点位泵
   - 在 `static_measure_time` 内连续采样
   - 目的：在尽量少耗气的情况下观察是否稳定

### 当前内部固定参数

下面两个参数目前是程序内部常量，不在配置文件里：

- 静态采样间隔：`5000 ms`
- 最小观察期：`10000 ms`

也就是：

- 停泵后不会立刻判稳
- 先经过最小观察期，再开始检查稳定性

## 判稳逻辑

### 判稳通道

当前只使用以下两个通道参与稳定判定：

- `CO2`
- `O2`

### 判稳方法

- 使用最近 `3` 个有效样本
- 分别计算 `CO2` 和 `O2` 的相对变化率 `%/min`
- 只有两者同时满足阈值，才认为进入稳态

### 当前阈值

- `CO2`
  `5.0 %/min`
- `O2`
  `1.5 %/min`

### 百分比计算参考下限

为了避免低读数下分母过小导致百分比失真，计算相对变化率时设置了参考下限：

- `CO2`
  最低按 `800 ppm`
- `O2`
  最低按 `5.0 %`

## 结果与质量标记

### 两套样本集合

程序维护两套样本：

- `stable`
  判稳后采集到的正式稳态样本
- `fallback`
  最小观察期之后采集到的兜底样本

### 最终结果怎么选

- 如果静态窗口结束时，`CO2` 和 `O2` 仍同时稳定
  - 使用 `stable`
- 如果静态窗口结束时，双通道没有同时稳定
  - 使用 `fallback`

### 稳健统计方法

最终值不是简单平均，而是抗异常统计：

- 1 个样本：直接使用
- 2 个样本：取平均
- 3 到 4 个样本：偏向中位数
- 更多样本：去掉头尾异常值后再做截尾均值

### `quality` 规则

- `CO2`
  - 最终稳定时按数值有效性给 `OK/ERR`
  - 最终不稳定时给 `UNSTABLE`
- `O2`
  - 最终稳定时按数值有效性给 `OK/ERR`
  - 最终不稳定时给 `UNSTABLE`
- `CO / H2S / CH4 / AirTemp / AirHumidity`
  - 不参与稳定判定
  - 只按数值有效性给 `OK/ERR`

说明：

- “是否使用正式稳态结果”
- “`CO2` / `O2` 是否标记 `UNSTABLE`”

这两件事现在使用同一套最终判定口径，不再互相矛盾。

## MQTT 设计

### MQTT 连接配置

示例：

```json
"mqtt": {
  "server": "118.25.108.254",
  "port": 1883,
  "user": "equipment",
  "pass": "ZNXK8888",
  "device_code": "MMCGS001",
  "point_device_codes": [
    "MMCGS001-P1",
    "MMCGS001-P2",
    "MMCGS001-P3",
    "MMCGS001-P4",
    "MMCGS001-P5",
    "MMCGS001-P6"
  ]
}
```

### 自动生成的 topic

控制器自身 topic：

- 注册上线：
  `compostlab/v2/{device_code}/register`
- 命令订阅：
  `compostlab/v2/{device_code}/response`

点位上报 topic：

- 遥测上报：
  `compostlab/v2/{point_device_code}/telemetry`

示例：

- 控制器：`MMCGS001`
- 点位 1：`MMCGS001-P1`

则：

- 注册 topic：
  `compostlab/v2/MMCGS001/register`
- 订阅 topic：
  `compostlab/v2/MMCGS001/response`
- 点位 1 遥测 topic：
  `compostlab/v2/MMCGS001-P1/telemetry`

## MQTT 上报

### 1. 上线注册消息

设备启动后会上报一条注册消息。

topic：

```text
compostlab/v2/{device_code}/register
```

payload：

```json
{
  "schema_version": 2,
  "ip_address": "x.x.x.x",
  "timestamp": "YYYY-MM-DD HH:MM:SS",
  "config": {
    "wifi": {
      "ssid": "...",
      "password": "..."
    },
    "mqtt": {
      "server": "...",
      "port": 1883,
      "user": "...",
      "pass": "...",
      "device_code": "...",
      "point_device_codes": ["...", "..."]
    },
    "ntp_servers": ["...", "..."],
    "sample_time": 15000,
    "static_measure_time": 30000,
    "purge_pump_time": 30000,
    "read_interval": 1200000
  }
}
```

说明：

- `ip_address` 优先尝试公网 IP，失败时退回局域网 IP
- `config` 为当前完整配置

### 2. 点位遥测消息

每个点位检测结束后会上报一条遥测消息。

topic：

```text
compostlab/v2/{point_device_code}/telemetry
```

payload：

```json
{
  "schema_version": 2,
  "ts": "YYYY-MM-DD HH:MM:SS",
  "point_id": 1,
  "controller_device_code": "MMCGS001",
  "channels": [
    {
      "code": "CO2",
      "value": 2.35,
      "unit": "%VOL",
      "quality": "OK"
    },
    {
      "code": "CO",
      "value": 12.0,
      "unit": "ppm",
      "quality": "OK"
    },
    {
      "code": "H2S",
      "value": 1.0,
      "unit": "ppm",
      "quality": "OK"
    },
    {
      "code": "O2",
      "value": 20.10,
      "unit": "%VOL",
      "quality": "UNSTABLE"
    },
    {
      "code": "CH4",
      "value": 0.0,
      "unit": "%LEL",
      "quality": "OK"
    },
    {
      "code": "AirTemp",
      "value": 28.6,
      "unit": "℃",
      "quality": "OK"
    },
    {
      "code": "AirHumidity",
      "value": 65.0,
      "unit": "%RH",
      "quality": "OK"
    }
  ]
}
```

字段说明：

- `ts`
  采样时间
- `point_id`
  点位编号，范围 `1 ~ 6`
- `controller_device_code`
  控制器编码，不是点位编码
- `channels`
  通道数组，每个通道包含：
  - `code`
  - `value`
  - `unit`
  - `quality`

补充说明：

- `CO2` 内部原始值来自 `ppm`
- 上报前会换算为 `%VOL`

## MQTT 下发

设备只监听控制器自己的响应 topic：

```text
compostlab/v2/{device_code}/response
```

收到消息后会先检查：

- JSON 是否合法
- 顶层 `device` 是否等于本机 `device_code`
- 是否存在 `commands` 数组

只有匹配本机的消息才会执行。

### 下发消息总格式

```json
{
  "device": "MMCGS001",
  "commands": [
    {
      "command": "restart"
    }
  ]
}
```

说明：

- `device` 必须等于本机 `device_code`
- `commands` 是数组，一条消息可以带多个命令

### 支持的命令

- `config_update`
- `update_config`
- `restart`
- `purge`
- `point1`
- `point2`
- `point3`
- `point4`
- `point5`
- `point6`

当前不再支持模糊的 `pump` 命令。

### 1. 配置更新命令

支持命令名：

- `config_update`
- `update_config`

示例：

```json
{
  "device": "MMCGS001",
  "commands": [
    {
      "command": "config_update",
      "config": {
        "sample_time": 15000,
        "static_measure_time": 30000,
        "purge_pump_time": 30000,
        "read_interval": 1200000,
        "wifi": {
          "ssid": "your-ssid",
          "password": "your-password"
        },
        "mqtt": {
          "server": "broker.example.com",
          "port": 1883,
          "user": "user",
          "pass": "pass",
          "clientId": "esp32",
          "device_code": "MMCGS001",
          "point_device_codes": [
            "MMCGS001-P1",
            "MMCGS001-P2",
            "MMCGS001-P3",
            "MMCGS001-P4",
            "MMCGS001-P5",
            "MMCGS001-P6"
          ]
        },
        "ntp_servers": [
          "ntp.aliyun.com",
          "cn.ntp.org.cn"
        ]
      }
    }
  ]
}
```

执行行为：

1. 只更新 `config` 中出现的字段
2. 保存到 `/config.json`
3. 设置延迟重启标记
4. 约 3 秒后自动重启

### 2. 重启命令

示例：

```json
{
  "device": "MMCGS001",
  "commands": [
    {
      "command": "restart"
    }
  ]
}
```

执行行为：

- 进入命令执行流程
- 触发设备重启

### 3. 泵控命令

支持的泵控命令：

- `purge`
- `point1`
- `point2`
- `point3`
- `point4`
- `point5`
- `point6`

支持字段：

- `command`
  命令名
- `action`
  `on` 或 `off`
- `duration`
  持续时间，单位毫秒
- `schedule`
  计划执行时间，格式 `YYYY-MM-DD HH:MM:SS`

示例 1：立即打开吹扫泵 10 秒

```json
{
  "device": "MMCGS001",
  "commands": [
    {
      "command": "purge",
      "action": "on",
      "duration": 10000
    }
  ]
}
```

示例 2：在指定时间打开 point3 15 秒

```json
{
  "device": "MMCGS001",
  "commands": [
    {
      "command": "point3",
      "action": "on",
      "duration": 15000,
      "schedule": "2026-04-02 18:30:00"
    }
  ]
}
```

示例 3：关闭 point2

```json
{
  "device": "MMCGS001",
  "commands": [
    {
      "command": "point2",
      "action": "off"
    }
  ]
}
```

### 手动泵控限制

自动巡检进行中时：

- 所有手动泵控命令都会被拒绝
- 这样可以避免打乱当前点位采样与吹扫气路

重启命令不受这个限制。

## 离线缓存与补传

如果 MQTT 发布失败：

- 数据不会直接丢弃
- 会尝试缓存到 SPIFFS
- 网络恢复后自动补传

### 缓存文件

- `/data_cache.json`
- `/data_cache.tmp`
- `/data_cache.bak`

### 补传时机

- 启动后补传最多 10 条
- 每轮巡检结束后补传最多 10 条
- `loop()` 中每 30 秒补传最多 10 条

### 当前缓存初始化参数

- 最大缓存条数：`200`
- 最大缓存保存天数：`7`

### 缓存淘汰策略

- 缓存满时优先淘汰最旧的已上传记录
- 如果没有已上传记录，则淘汰最旧的待上传记录

## NVS 恢复机制

项目会把巡检进度保存到 NVS，以便异常重启后尽量续跑。

### 保存的内容

- 上一轮巡检开始时间
- 是否存在待恢复巡检
- 中断点位
- 中断阶段
- 当前轮最近已完成点位

### 恢复策略

- 如果存在未完成巡检，则优先从中断点续跑
- 如果没有未完成巡检，则根据上次巡检开始时间恢复 `read_interval` 节奏
- 如果已经超过一个周期，则开机后立即开始新一轮

## 网络维护策略

- WiFi 断线会自动重连
- 网络连通性会定期检测
- MQTT 断线会自动重连
- MQTT 重连成功后会自动重新订阅控制响应 topic
- 网络异常持续过久时，系统会主动重启尝试恢复

## 已知约束

- 静态采样间隔和最小观察期目前还是内部固定值，不是配置项
- 稳定判定目前只使用 `CO2 + O2`
- 其它通道不参与稳定判定
- Flash 占用已经比较高，后续加功能要注意空间
- `data/config.json` 可能包含真实账号密码，提交前建议脱敏

## 维护约定

后续只要修改了以下任一内容，都必须同步更新本 README：

- 检测流程
- 判稳逻辑
- 结果与 `quality` 规则
- MQTT 上报格式
- MQTT 下发格式
- 配置项定义
- 缓存策略
- 恢复逻辑
- 硬件引脚或泵路结构
- 手动命令与自动检测的互斥规则

如果代码和 README 不一致，以代码为准，但修改代码后必须补 README。

## 变更记录

### 2026-04-02

- 检测流程从“长时间边抽边测”改为“短时取样抽气 + 停泵静态检测”
- 新增 `static_measure_time`
- 判稳逻辑改为 `CO2 + O2` 双通道共同判稳
- `CO2` 与 `O2` 的 `UNSTABLE` 改为独立判断
- 自动巡检进行中拒绝手动泵控命令
- 去掉有歧义的 `pump` 命令映射，只保留 `point1 ~ point6` 和 `purge`
- 正式稳态结果与 `UNSTABLE` 标记统一按最终稳定状态判断
- 修复 `duration=0` 时泵可能保持常开的 bug
- README 重写，补齐 MQTT 上报与下发协议说明
