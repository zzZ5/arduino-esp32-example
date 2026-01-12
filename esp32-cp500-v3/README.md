# ESP32 浴槽加热/循环泵控制器

## 项目简介

基于 ESP32 的智能浴槽加热与循环泵控制系统，支持两种温控模式：
- **n-curve 模式**：基于内外温差的自适应补热控制
- **Setpoint 模式**：基于目标温度的定点恒温控制

## 功能特性

### 核心控制功能
- 🌡️ **多探头温度采集**：核心温度、外浴多探头（中位去噪）、水箱温度
- 🔥 **加热器控制**：支持最短开/关机时间保护，防止频繁启停
- 💧 **循环泵控制**：支持与加热器并行运行，智能热能分配
- 💨 **曝气控制**：支持定时曝气，可配置间隔和持续时间
- 🎯 **双温控模式**：
  - n-curve：根据 t_in 与 t_out 差值自适应补热
  - Setpoint：根据目标温度和回差精确控温

### 安全保护
- 🛡️ **水箱温度保护**：水箱过热或无效时强制停止加热
- 🌡️ **外浴温度上限**：超过上限时强制冷却（关加热+关泵）
- ⏱️ **最小开/关机时间**：保护加热器，延长设备寿命
- 📊 **ADAPTIVE_TOUT 学习**：根据上一轮泵助热效果自适应调整阈值

### 通信与配置
- 📶 **WiFi 连接**：支持自动重连
- ⏰ **NTP 时间同步**：多服务器同步，自动时区设置
- 📨 **MQTT 通信**：
  - 设备遥测上报（schema_version 2）
  - 命令接收与队列执行
  - 支持配置远程更新
- 💾 **SPIFFS 配置存储**：配置文件可持久化保存
- 🔄 **NVS 相位恢复**：重启后恢复测量/曝气节拍

## 硬件连接

| 功能 | GPIO | 说明 |
|-----|------|------|
| 内部温度传感器 (DS18B20) | 4 | OneWire 总线，核心温度 |
| 外部温度传感器 (DS18B20) | 5 | OneWire 总线，外浴多探头 |
| 加热器控制 | 25 | 继电器输出 |
| 循环泵控制 | 26 | 继电器输出 |
| 曝气控制 | 27 | PWM 输出，支持软启动 |

## 软件依赖

- **PlatformIO**: >= 6.0
- **Arduino Framework**
- **依赖库**:
  - `bblanchon/ArduinoJson@^7.4.2`
  - `knolleary/PubSubClient@^2.8`
  - `milesburton/DallasTemperature@^4.0.4`

## 编译与烧录

### 前置要求
1. 安装 PlatformIO
2. 安装 ESP32 开发板支持
3. 安装 VS Code + PlatformIO 插件（推荐）

### 编译步骤
```bash
cd esp32-cp500-v3
pio run
```

### 烧录到设备
```bash
pio run --target upload
```

### 上传配置文件（data 目录）
```bash
pio run --target uploadfs
```

## 配置说明

配置文件路径：`data/config.json`

### 配置文件完整示例

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
    "device_code": "H3w8flPrdA"
  },
  "post_interval": 60000,
  "ntp_host": [
    "ntp.ntsc.ac.cn",
    "ntp.aliyun.com",
    "cn.ntp.org.cn",
    "ntp.tuna.tsinghua.edu.cn",
    "ntp.sjtu.edu.cn",
    "202.120.2.101"
  ],
  "temp_limitout_max": 65,
  "temp_limitin_max": 70,
  "temp_limitout_min": 25,
  "temp_limitin_min": 25,
  "temp_maxdif": 13,
  "bath_setpoint": {
    "enabled": true,
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

### 详细参数说明

#### 1. WiFi 配置

| 参数 | 类型 | 默认值 | 说明 | 调整建议 |
|-----|------|--------|------|---------|
| `wifi.ssid` | String | "Compostlab" | WiFi 网络名称 | 修改为你的 WiFi 名称 |
| `wifi.password` | String | "znxk8888" | WiFi 密码 | 修改为你的 WiFi 密码 |

**注意**：
- 仅支持 2.4GHz WiFi
- 确保设备在 WiFi 信号覆盖范围内

#### 2. MQTT 配置

| 参数 | 类型 | 默认值 | 说明 | 调整建议 |
|-----|------|--------|------|---------|
| `mqtt.server` | String | "118.25.108.254" | MQTT 服务器地址 | 修改为你的 MQTT 服务器 IP 或域名 |
| `mqtt.port` | Number | 1883 | MQTT 服务器端口 | 通常使用 1883（非加密）或 8883（SSL） |
| `mqtt.user` | String | "equipment" | MQTT 用户名 | 使用你的 MQTT 服务器的用户名 |
| `mqtt.pass` | String | "ZNXK8888" | MQTT 密码 | 使用你的 MQTT 服务器的密码 |
| `mqtt.device_code` | String | "H3w8flPrdA" | 设备唯一编码 | **必须唯一**，用于生成 Topic |

**MQTT Topic 自动生成规则**：
- 上报 Topic：`compostlab/v2/{device_code}/telemetry`
- 响应 Topic：`compostlab/v2/{device_code}/response`

**注意**：
- `device_code` 必须在系统中唯一，不同设备不能重复
- 修改 `device_code` 后需重启设备

#### 3. NTP 时间服务器配置

| 参数 | 类型 | 说明 |
|-----|------|------|
| `ntp_host` | Array | NTP 服务器地址列表，支持多个备用服务器 |

**默认服务器列表**：
- ntp.ntsc.ac.cn（中科院）
- ntp.aliyun.com（阿里云）
- cn.ntp.org.cn（中国公共 NTP）
- ntp.tuna.tsinghua.edu.cn（清华大学）
- ntp.sjtu.edu.cn（上海交大）
- 202.120.2.101（备用 IP）

**调整建议**：
- 一般使用默认列表即可
- 如果网络环境特殊，可替换为本地 NTP 服务器
- 至少保留一个可用的 NTP 服务器

#### 4. 测控周期配置

| 参数 | 类型 | 默认值 | 单位 | 说明 | 调整建议 |
|-----|------|--------|------|------|---------|
| `post_interval` | Number | 60000 | 毫秒 | 温度测量和控制周期 | 30-120 秒为宜，太短增加系统负担，太长响应慢 |

**计算参考**：
- 30000ms = 30 秒
- 60000ms = 60 秒（推荐）
- 120000ms = 120 秒

#### 5. 温度限值配置

| 参数 | 类型 | 默认值 | 单位 | 说明 | 调整建议 |
|-----|------|--------|------|------|---------|
| `temp_limitout_max` | Number | 65 | ℃ | 外浴温度上限 | 根据实际工艺需求设置，65-75℃ 较常见 |
| `temp_limitin_max` | Number | 70 | ℃ | 核心温度上限 | 用于 n-curve 模式的归一化，通常比外浴上限高 5-10℃ |
| `temp_limitout_min` | Number | 25 | ℃ | 外浴温度下限 | 用于 n-curve 模式的归一化 |
| `temp_limitin_min` | Number | 25 | ℃ | 核心温度下限 | 用于 n-curve 模式的归一化 |
| `temp_maxdif` | Number | 13 | ℃ | n-curve 最大温差阈值 | 影响 n-curve 模式补热敏感度，越大越不敏感 |

**参数关系**：
- `in_max` - `in_min`：核心温度控制范围
- `out_max` - `out_min`：外浴温度控制范围
- `temp_maxdif`：n-curve 模式中 t_in - t_out 的最大允许差值

**调整建议**：
- 外浴温度超过 `temp_limitout_max` 时会触发硬保护（关加热+关泵）
- Setpoint 模式会自动限制目标温度不超过 `temp_limitout_max - 0.2℃`
- n-curve 模式的灵敏度由 `temp_maxdif` 控制，值越小越敏感

#### 6. Setpoint 模式配置

| 参数 | 类型 | 默认值 | 单位 | 说明 | 调整建议 |
|-----|------|--------|------|------|---------|
| `bath_setpoint.enabled` | Boolean | true | - | 是否启用 Setpoint 模式 | true=Setpoint 模式，false=n-curve 模式 |
| `bath_setpoint.target` | Number | 65.0 | ℃ | 目标温度 | 根据工艺需求设置，建议 50-70℃ |
| `bath_setpoint.hyst` | Number | 0.8 | ℃ | 回差温度 | 防止频繁启停，0.5-1.0℃ 较合适 |

**工作原理**：
- 外浴温度低于 `target - hyst` 时开始加热
- 外浴温度高于 `target + hyst` 时停止加热
- 温度在 `[target - hyst, target + hyst]` 范围内时保持当前状态

**调整建议**：
- `target` 是你希望维持的温度
- `hyst` 过小会导致设备频繁启停，过大会导致温度波动大
- 温度精度要求高时，适当减小 `hyst`

#### 7. 曝气定时配置

| 参数 | 类型 | 默认值 | 单位 | 说明 | 调整建议 |
|-----|------|--------|------|------|---------|
| `aeration_timer.enabled` | Boolean | true | - | 是否启用定时曝气 | true=启用，false=禁用 |
| `aeration_timer.interval` | Number | 300000 | 毫秒 | 曝气间隔时间 | 10-30 分钟较常见 |
| `aeration_timer.duration` | Number | 300000 | 毫秒 | 每次曝气持续时间 | 2-10 分钟较常见 |

**计算参考**：
- `interval`: 300000ms = 5 分钟，600000ms = 10 分钟
- `duration`: 300000ms = 5 分钟，600000ms = 10 分钟

**调整建议**：
- 曝气用于增加溶氧量，防止厌氧环境
- 间隔时间短、持续时间长 → 曝气充分但能耗高
- 间隔时间长、持续时间短 → 节能但可能曝气不足
- 手动曝气命令会临时覆盖定时配置

#### 8. 安全保护配置

| 参数 | 类型 | 默认值 | 单位 | 说明 | 调整建议 |
|-----|------|--------|------|------|---------|
| `safety.tank_temp_max` | Number | 90.0 | ℃ | 水箱温度上限 | 80-95℃ 较常见，根据水箱规格设置 |
| `heater_guard.min_on_ms` | Number | 30000 | 毫秒 | 加热器最短开机时间 | 20-60 秒，防止频繁启停 |
| `heater_guard.min_off_ms` | Number | 30000 | 毫秒 | 加热器最短关机时间 | 20-60 秒，保护设备寿命 |

**工作原理**：
- 水箱温度超过 `tank_temp_max` 时强制关闭加热器（最高优先级）
- 加热器开启后至少运行 `min_on_ms` 才能关闭
- 加热器关闭后至少等待 `min_off_ms` 才能再次开启

**调整建议**：
- `tank_temp_max` 应该明显高于目标温度，留出安全裕度
- 大功率加热器可以适当缩短 `min_on_ms`
- 环境温度低时可以适当缩短 `min_off_ms`

#### 9. 泵自适应配置

| 参数 | 类型 | 默认值 | 单位 | 说明 | 调整建议 |
|-----|------|--------|------|------|---------|
| `pump_adaptive.delta_on_min` | Number | 6.0 | ℃ | 最小开启温差 | 核心温度低时使用，值越小越易开启泵 |
| `pump_adaptive.delta_on_max` | Number | 25.0 | ℃ | 最大开启温差 | 核心温度高时使用，值越大越难开启泵 |
| `pump_adaptive.hyst_nom` | Number | 3.0 | ℃ | 名义回差 | 用于计算 Δ_off，影响泵的保持区间 |
| `pump_adaptive.ncurve_gamma` | Number | 1.3 | - | n-curve 指数 | 影响 Δ_on 随 t_in 变化的曲线形状 |

**工作原理**（n-curve 模式）：
- 根据 t_in 在 `[in_min, in_max]` 的位置计算 Δ_on
- t_in 低 → Δ_on 小（容易开启泵）
- t_in 高 → Δ_on 大（不易开启泵）
- Δ_off = Δ_on × (hyst_nom / 中间值)

**调整建议**：
- `ncurve_gamma` = 1.0：线性变化
- `ncurve_gamma` > 1.0：低 t_in 时 Δ_on 增长慢（更早开泵）
- `ncurve_gamma` < 1.0：高 t_in 时 Δ_on 增长慢（更晚开泵）
- `hyst_nom` 影响泵的稳定性，值越大保持时间越长

#### 10. 泵学习配置

| 参数 | 类型 | 默认值 | 单位 | 说明 | 调整建议 |
|-----|------|--------|------|------|---------|
| `pump_learning.step_up` | Number | 0.5 | ℃ | 学习上调步长 | 0.3-1.0℃ |
| `pump_learning.step_down` | Number | 0.2 | ℃ | 学习下调步长 | 0.1-0.5℃ |
| `pump_learning.max` | Number | 10.0 | ℃ | 学习补偿上限 | 5-15℃ |
| `pump_learning.progress_min` | Number | 0.05 | ℃ | 最小升温阈值 | 判断泵是否有效的标准 |

**工作原理**（ADAPTIVE_TOUT）：
- 上一周期仅泵运行（heater 关闭）时：
  - 如果 `t_out 升温 >= progress_min` → 泵有效，Δ_on 减少 `step_down`
  - 如果 `t_out 升温 < progress_min` → 泵无效，Δ_on 增加 `step_up`
- 上一周期非仅泵运行时 → Δ_on 减少 `step_down`
- 学习补偿 `gPumpDeltaBoost` 在 `[0, max]` 范围内

**调整建议**：
- `progress_min` 太大会导致误判泵无效，太小会导致误判有效
- `step_up` 应该大于 `step_down`，这样能快速响应无效情况
- `max` 限制了最大补偿，防止过度学习

#### 11. n-curve 曲线配置

| 参数 | 类型 | 默认值 | 单位 | 说明 | 调整建议 |
|-----|------|--------|------|------|---------|
| `curves.in_diff_ncurve_gamma` | Number | 2.0 | - | n-curve 指数 | 影响温差阈值随 t_in 变化的曲线 |

**工作原理**（n-curve 模式）：
- 根据 t_in 在 `[in_min, in_max]` 的位置计算温差阈值 `DIFF_THR`
- `DIFF_THR = diff_min + (diff_max - diff_min) × u^ncurve_gamma`
- 其中 `u = (t_in - in_min) / (in_max - in_min)` 是归一化位置（0~1）
- 当 `t_in - t_out > DIFF_THR` 时需要补热

**调整建议**：
- `ncurve_gamma` = 1.0：DIFF_THR 随 t_in 线性增加
- `ncurve_gamma` > 1.0：t_in 低时 DIFF_THR 增长慢（更敏感）
- `ncurve_gamma` < 1.0：t_in 高时 DIFF_THR 增长慢（更保守）

### 配置修改方式

#### 方式一：直接编辑配置文件

1. 打开 `data/config.json` 文件
2. 修改需要的参数
3. 保存文件
4. 通过 PlatformIO 上传到设备：
   ```bash
   pio run --target uploadfs
   ```
5. 重启设备使配置生效

#### 方式二：MQTT 远程更新

发送配置更新命令：

```json
{
  "device": "H3w8flPrdA",
  "commands": [
    {
      "command": "config_update",
      "config": {
        "bath_setpoint": {
          "enabled": false
        }
      }
    }
  ]
}
```

**注意**：
- 只需发送要修改的部分，未指定的字段保持不变
- 配置更新后设备会自动重启
- 部分参数（如 WiFi、MQTT）建议使用方式一修改

### 配置验证

修改配置后，可以通过以下方式验证：

1. **查看 Serial 输出**：
   - 设备启动时会打印完整配置
   - 检查各参数是否正确加载

2. **查看 MQTT 上线消息**：
   - 上线消息的 `other` 字段包含所有控制参数
   - 确认参数值是否符合预期

3. **测试设备响应**：
   - 手动发送命令，检查设备是否正确执行
   - 观察温度变化是否符合配置的逻辑

### 常见配置场景

#### 场景一：低温恒温控制（45℃）
```json
{
  "bath_setpoint": {
    "enabled": true,
    "target": 45.0,
    "hyst": 0.8
  },
  "temp_limitout_max": 50,
  "temp_limitin_max": 55
}
```

#### 场景二：高温快速加热（70℃）
```json
{
  "bath_setpoint": {
    "enabled": true,
    "target": 70.0,
    "hyst": 1.0
  },
  "temp_limitout_max": 75,
  "temp_limitin_max": 80,
  "safety": {
    "tank_temp_max": 95.0
  }
}
```

#### 场景三：自适应节能模式（n-curve）
```json
{
  "bath_setpoint": {
    "enabled": false
  },
  "temp_maxdif": 15,
  "pump_adaptive": {
    "delta_on_min": 8.0,
    "delta_on_max": 20.0,
    "hyst_nom": 2.5
  }
}
```

#### 场景四：高频曝气模式
```json
{
  "aeration_timer": {
    "enabled": true,
    "interval": 600000,
    "duration": 120000
  }
}
```

## MQTT 通信协议

### 上报格式 (Telemetry)

**Topic**: `compostlab/v2/{device_code}/telemetry`

**格式**:
```json
{
  "schema_version": 2,
  "ts": "2026-01-11 12:00:00",
  "channels": [
    {"code": "TempIn", "value": 45.5, "unit": "℃", "quality": "ok"},
    {"code": "TempOut1", "value": 65.2, "unit": "℃", "quality": "ok"},
    {"code": "TempOut2", "value": 65.3, "unit": "℃", "quality": "ok"},
    {"code": "TempOut3", "value": 65.1, "unit": "℃", "quality": "ok"},
    {"code": "TankTemp", "value": 80.0, "unit": "℃", "quality": "ok"},
    {"code": "Heater", "value": 1, "unit": "", "quality": "ok"},
    {"code": "Pump", "value": 1, "unit": "", "quality": "ok"},
    {"code": "Aeration", "value": 0, "unit": "", "quality": "ok"}
  ]
}
```

### 上线消息 (Boot)

**Topic**: `compostlab/v2/{device_code}/telemetry`

**格式**:
```json
{
  "schema_version": 2,
  "ts": "2026-01-11 12:00:00",
  "device": "H3w8flPrdA",
  "status": "online",
  "last_measure_time": "2026-01-11 11:59:30",
  "other": {
    "mode": "setpoint",
    "post_interval": 60000,
    "temp_limits": {
      "out_max": 65,
      "out_min": 25,
      "in_max": 70,
      "in_min": 25
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
    "bath_setpoint": {
      "enabled": true,
      "target": 65.0,
      "hyst": 0.8
    },
    "aeration_timer": {
      "enabled": true,
      "interval": 300000,
      "duration": 300000
    }
  }
}
```

### 命令接收 (Command)

**Topic**: `compostlab/v2/{device_code}/response`

**格式**:
```json
{
  "device": "H3w8flPrdA",
  "commands": [
    {
      "command": "heater",
      "action": "on",
      "duration": 300000
    },
    {
      "command": "pump",
      "action": "on",
      "duration": 60000
    },
    {
      "command": "fan",
      "action": "on",
      "duration": 120000
    }
  ]
}
```

### 支持的命令类型

| 命令 | 说明 |
|-----|------|
| `heater` | 加热器控制 |
| `pump` | 水泵控制 |
| `fan` | 曝气控制（映射到 aeration） |
| `aeration` | 曝气控制（兼容） |

## 温控模式详解

### n-curve 模式

根据核心温度（t_in）与外浴温度（t_out）的差值自适应补热。

**工作原理**:
1. 计算温差：`diff_now = t_in - t_out`
2. 根据 t_in 位置计算动态阈值：
   - t_in 低时，阈值较小，容易触发补热
   - t_in 高时，阈值较大，减少补热
3. 当 `diff_now > 阈值` 时，需要补热
4. 结合水箱热差（`delta_tank_out = t_tank - t_out`）决定加热+泵组合：
   - `delta_tank_out > Δ_on`：加热器 + 泵同时运行
   - `Δ_off < delta_tank_out < Δ_on`：仅保持当前泵状态
   - `delta_tank_out < Δ_off`：仅加热器运行

**特点**:
- 无预测性，纯响应式控制
- n-curve 参数可调：`in_diff_ncurve_gamma`
- 支持学习补偿：`gPumpDeltaBoost` 自适应调整

### Setpoint 模式

根据目标温度（target）和回差（hyst）精确控温。

**工作原理**:
1. 判断外浴温度状态：
   - `t_out < target - hyst`：需要加热
   - `t_out > target + hyst`：需要冷却
   - 其他：温度合适，保持
2. 加热时结合水箱热差：
   - `t_tank < target + Δ_on`：水箱偏冷，仅加热水箱
   - `t_tank >= target + Δ_on`：水箱有热，加热+泵同时运行

**特点**:
- 精确控温，适合恒温需求
- 回差防止频繁启停：`bath_setpoint.hyst`
- 可动态切换：远程修改 `bath_setpoint.enabled`

### 模式切换

通过修改 `config.json` 中的 `bath_setpoint.enabled` 字段：
- `true` → Setpoint 模式
- `false` → n-curve 模式

配置修改后需重启生效，或通过 MQTT `config_update` 命令远程更新。

## 安全机制

### 水箱温度保护
- 水箱温度超过 `tank_temp_max` 时，强制关闭加热器
- 水箱温度无效（NaN 或超出合理范围）时，停止自动加热
- 手动加热命令也会被安全拦截

### 外浴温度上限
- 外浴温度超过 `temp_limitout_max` 时：
  - 强制关闭加热器
  - 强制关闭循环泵
  - 清除手动软锁
- 这是最高优先级的安全保护

### 加热器防抖
- 最短开机时间：`heater_min_on_ms`（默认 30 秒）
- 最短关机时间：`heater_min_off_ms`（默认 30 秒）
- 防止频繁启停损坏设备

### 手动控制软锁
- 加热/泵/曝气手动命令后设置软锁
- 自动控制逻辑不会主动改变被锁定的设备
- 软锁在 `duration` 到期后自动释放
- 持续时间为 0 时表示永久锁定

## 故障排查

### 设备无法连接 WiFi
- 检查 `config.json` 中的 WiFi 配置
- 查看 Serial 输出中的连接状态
- 确认路由器工作正常

### MQTT 连接失败
- 检查 MQTT 服务器地址和端口
- 确认用户名和密码正确
- 检查防火墙设置

### 温度读取异常
- 检查 DS18B20 传感器接线
- 确认上拉电阻（4.7kΩ）
- 查看 Serial 输出的温度值

### 设备频繁重启
- 检查供电稳定性
- 查看 Serial 输出的错误信息
- 确认配置文件格式正确

## 文件结构

```
esp32-cp500-v3/
├── src/                    # 源代码
│   ├── main.cpp            # 主程序
│   ├── config_manager.h     # 配置管理头文件
│   ├── config_manager.cpp   # 配置管理实现
│   ├── sensor.h            # 传感器接口
│   ├── sensor.cpp          # 传感器实现
│   ├── wifi_ntp_mqtt.h    # 网络和 MQTT 头文件
│   └── wifi_ntp_mqtt.cpp  # 网络和 MQTT 实现
├── data/                  # SPIFFS 数据文件
│   └── config.json        # 配置文件
├── platformio.ini          # PlatformIO 配置
└── README.md              # 本文档
```

## 技术支持

如有问题或建议，请联系技术支持。

## 版本历史

| 版本 | 日期 | 说明 |
|-----|------|------|
| 3.0 | 2026-01-11 | 重构为 schema_version 2，支持 channels 数组格式 |
| 2.0 | 2026-01-11 | 简化配置，自动生成 MQTT topic |
| 1.0 | 2026-01-11 | 初始版本 |

## 许可证

本项目仅供学习和参考使用。
