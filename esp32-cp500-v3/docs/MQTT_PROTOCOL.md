# MQTT 协议文档

本文档描述 `esp32-cp500-v3` 当前固件版本使用的 MQTT Topic、消息结构和命令语义，适合联调、平台接入和第三方系统对接使用。

## Topic 约定

设备编码记为 `{device_code}`，由配置项 `mqtt.device_code` 决定。

| 方向 | Topic | 说明 |
|-----|------|------|
| 设备上报 | `compostlab/v2/{device_code}/telemetry` | 周期性遥测数据 |
| 设备上线 | `compostlab/v2/{device_code}/register` | 启动完成后的注册/上线消息 |
| 平台下发 | `compostlab/v2/{device_code}/response` | 控制命令与远程配置命令 |

## 1. 遥测上报

### Topic

`compostlab/v2/{device_code}/telemetry`

### 示例

```json
{
  "schema_version": 2,
  "ts": "2026-04-02 10:00:00",
  "channels": [
    { "code": "TempIn", "value": 45.5, "unit": "C", "quality": "ok" },
    { "code": "TempTank", "value": 52.1, "unit": "C", "quality": "ok" },
    { "code": "Heater", "value": 1, "unit": "", "quality": "ok" },
    { "code": "Pump", "value": 0, "unit": "", "quality": "ok" },
    { "code": "Aeration", "value": 1, "unit": "", "quality": "ok" }
  ]
}
```

### 字段说明

| 字段 | 类型 | 说明 |
|-----|------|------|
| `schema_version` | Number | 当前为 `2` |
| `ts` | String | 设备本地时间戳 |
| `channels` | Array | 通道数组 |
| `channels[].code` | String | 通道编码 |
| `channels[].value` | Number/String | 通道值 |
| `channels[].unit` | String | 单位，可为空 |
| `channels[].quality` | String | 数据质量，常见为 `ok`、`ERR`、`NaN` |

常见通道编码包括：

- `TempIn`
- `TempOut1`、`TempOut2` ...
- `TempTank`
- `Heater`
- `Pump`
- `Aeration`

实际通道集合会随当前传感器数量和运行模式变化。

## 2. 上线消息

### Topic

`compostlab/v2/{device_code}/register`

### 说明

设备启动并完成初始化后会发送一条上线消息，内容包含设备基础信息和当前配置快照，便于平台识别设备状态。

### 安全说明

上线消息中的 WiFi 和 MQTT 密码字段不会明文上报，当前固件会使用掩码值替代。

## 3. 命令下发

### Topic

`compostlab/v2/{device_code}/response`

### 通用包结构

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

`commands` 是数组，一次可以批量下发多条命令，设备会按队列顺序执行。

## 4. 手动控制命令

适用命令：

- `heater`
- `pump`
- `aeration`
- `fan`

说明：`fan` 在设备内部会映射为 `aeration`。

### `action` 语义

| `action` | 说明 |
|-----|------|
| `on` | 打开设备，并进入手动锁状态 |
| `off` | 关闭设备，并清除该设备的手动锁和待执行定时命令 |
| `auto` | 解除该设备的手动锁，让自动控制重新接管 |

### `duration` 语义

| `duration` | 说明 |
|-----|------|
| `> 0` | 临时手动锁，持续到超时后自动补发对应 `off` |
| `= 0` | 持续手动锁，不自动释放 |
| 未提供 | 等同于 `0` |

### 关键行为规则

- 手动锁生效期间，自动控制不会主动改动对应设备状态。
- 新的同设备手动命令会清掉旧的待执行命令，避免历史 `off` 残留误触发。
- `auto` 不强制开或关设备，只是释放手动锁，自动控制会在下一周期接管。
- `heater on` 仍受水箱温度安全保护约束；当 Tank 温度无效或过温时，命令会被拒绝。

### 示例 1：加热器临时手动开启 5 分钟

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

### 示例 2：水泵持续手动开启

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

### 示例 3：解除曝气手动锁并恢复自动控制

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

## 5. 急停命令

### 激活急停

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

### 解除急停

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

### 急停行为

- 立即关闭所有执行设备。
- 阻断自动控制和普通手动控制命令。
- 设备仍会继续进行测量和数据上报。
- 只有明确收到 `emergency off` 后才会解除锁定。

## 6. 远程配置更新

### 命令格式

```json
{
  "commands": [
    {
      "command": "config_update",
      "config": {
        "post_interval": 60000
      }
    }
  ]
}
```

### 说明

- 设备会尝试更新内存中的配置。
- 更新成功后会写入 `/config.json`。
- 配置保存成功后设备会自动重启，使新配置生效。

## 7. 对接建议

- 平台下发命令时建议一次只控制一个目标设备，便于观察设备行为。
- 如果要把设备从人工干预恢复到自动控制，请显式发送 `action: "auto"`。
- 不建议依赖上线消息中的敏感字段；当前固件不会上报明文密码。
