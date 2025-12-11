# 水浴控温·微型堆肥反应器（ESP32）运行与运维手册

## 1. 系统概览

* **控制对象**：外部水浴（多探头取中位去噪）、堆体内温、上游水箱温度
* **核心策略**

  * 模式A：**n-curve 差阈控温**（默认，按 t_in 与 t_out 的差值单阈值判定是否需要补热；带“仅泵助热”自适应与学习补偿）
  * 模式B：**外浴定置控温（Setpoint）**（新增，可远程配置 `target/hyst`，按外浴中位温闭环控制）
* **互斥控制**：加热器与水泵绝不同时运行
* **辅助策略**：

  * “仅泵助热”：当水箱→外浴热差足够时，停加热只开泵以节能
  * 曝气**定时器**与**手动**两套触发路径
  * **软锁**：手动加热/手动泵/手动曝气在设定时长内优先生效
  * **硬保护**：外浴超上限、Tank 超上限立即关机/停止动作
* **通信**：MQTT（上线消息、周期上报、接收远程命令/配置）

---

## 2. 硬件与引脚

* **平台**：ESP32（Arduino Core 2.x）
* **必备库**：`ArduinoJson`、`Preferences`（随Core）、`FreeRTOS`（随Core）+ 你项目内模块：

  * `config_manager.*`（配置读写）
  * `wifi_ntp_mqtt.*`（联网、NTP、MQTT）
  * `sensor.*`（传感器采集：t_in、t_out[]、t_tank）
  * `log_manager.*`（日志/串口打印）

> 传感器与执行器实际引脚由你的 `initSensors(4, 5, 25, 26, 27)` 内部定义并统一初始化。

---

## 3. 固件构建与刷写

* **开发板**：ESP32 Dev Module（或对应型号）
* **串口**：115200
* **文件系统**：SPIFFS（首次需“上传文件系统映像”或通过MQTT远程下发 `config_update`）
* **启动流程**：

  1. 挂载SPIFFS → 读取 `/config.json`（无则用默认）
  2. 连接Wi-Fi → 同步NTP → 连接MQTT → 订阅 `response_topic`
  3. 初始化传感器 → 读取NVS时间 → 计算周期相位 → 发送上线消息
  4. 后台任务启动：`MeasureTask`（采集/控制/上报）、`CommandTask`（非阻塞命令执行）

---

## 4. 运行逻辑（控制要点）

### 4.1 探头融合

* 外浴温度：多探头→**中位数** + 可选**离群剔除**（±5℃ 默认）
* 内核温度（t_in）、水箱（tank）独立读数；Tank用于“仅泵助热”和安全上限判断

### 4.2 控温模式选择

* **Setpoint 模式优先**：`bath_setpoint.enabled=true` 时生效；否则走 **n-curve 模式**
* **硬保护**永远最高优先级：外浴 ≥ `temp_limitout_max` → 强制关加热&关泵；Tank≥上限 → 停热

### 4.3 Setpoint 模式（外浴定置）

* 目标：`bath_setpoint.target`（°C）；回差死区：`±bath_setpoint.hyst`
* **低于 (target − hyst)**：优先“仅泵助热”（若 `tank - t_out_med ≥ Δ_on`），否则开加热
* **高于 (target + hyst)**：全停自然冷却
* **死区内**：保持当前停机，避免抖动
* 仍结合“仅泵学习补偿”（提升 Δ_on 以提高仅泵进入门槛、节能但确保有效升温）

### 4.4 n-curve 模式（差阈控温）

* 针对 `diff_now = t_in − t_out_med`，按 t_in 在 `[temp_limitin_min, temp_limitin_max]` 区间的归一化位置计算**单阈值**
* `diff_now > 阈值` → “希望补热”；但若Tank热差不够，会先**加热水箱**至可“仅泵助热”
* 泵/热互斥、最小开停时间抑制、软锁同上

### 4.5 仅泵助热·自适应与学习

* **自适应阈值**：`Δ_on / Δ_off` 随 t_in 位置和名义回差比例变化
* **学习补偿**：若“仅泵”后外浴升温未达 `pumpProgressMin`，逐步提高进入“仅泵”的门槛（`pumpDeltaBoost`），反之缓慢回落
* 目的：避免“无效仅泵”浪费时间，提高节能策略稳定性

### 4.6 定时曝气

* 由 `aeration_timer` 控制（Enable / Interval / Duration）
* 支持手动命令覆盖，手动软锁有效期内自动逻辑不干预

---

## 5. 配置文件 `config.json`（字段与示例）

> 文件路径：`/config.json`（SPIFFS）。可本地写入，也可通过 MQTT `config_update` 远程更新并自动重启。

### 5.1 示例（基于你提供的配置，补入 `bath_setpoint`）

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
    "clientId": "cp500_006",
    "post_topic": "compostlab/O5DGVbateN/post/data",
    "response_topic": "compostlab/O5DGVbateN/response"
  },
  "keys": {
    "temp_in": "mqEGyxQVU56KPye",
    "temp_out": ["wBshNTETXYHbTXE", "dD8Hv9dOn0I8cFC", "E9Pvk6dC0RRrZJq"]
  },
  "post_interval": 60000,
  "ntp_host": [
    "ntp.ntsc.ac.cn","ntp.aliyun.com","cn.ntp.org.cn",
    "ntp.tuna.tsinghua.edu.cn","ntp.sjtu.edu.cn","202.120.2.101"
  ],
  "temp_maxdif": 5,
  "temp_limitout_max": 65,
  "temp_limitin_max": 70,
  "temp_limitout_min": 25,
  "temp_limitin_min": 25,
  "equipment_key": "O5DGVbateN",

  "aeration_timer": {
    "enabled": true,
    "interval": 0,
    "duration": 60000000000
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
    "delta_on_max": 30.0,
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
  },

  "bath_setpoint": {
    "enabled": true,
    "target": 45.0,
    "hyst": 0.8
  }
}
```

### 5.2 字段说明（要点）

* `wifi.*` / `mqtt.*` / `ntp_host[]`：联网与消息通道参数
* `post_interval`（ms）：采集-控制-上报周期
* `temp_limit*`（°C）：内/外温上下限；**外温上限**触发硬保护
* `keys.*`：上报数据字段Key（与你上位数据平台约定）
* `aeration_timer.*`：定时曝气（`interval/duration` 单位均 ms）
* `safety.tank_temp_max`：水箱安全上限（°C），超限立即停热
* `heater_guard.min_on/off_ms`：最小开/停机时间，抑制抖动
* `pump_adaptive.*`：仅泵自适应 Δ_on 范围、名义回差、n-curve指数
* `pump_learning.*`：仅泵学习补偿步长与上限、进度阈值
* `curves.in_diff_ncurve_gamma`：n-curve差阈指数
* `bath_setpoint.enabled/target/hyst`：定置外浴中位温及回差死区

---

## 6. MQTT 消息规范

### 6.1 上线消息（Boot）

* **Topic**：`post_topic`
* **Payload**：

```json
{
  "device": "O5DGVbateN",
  "status": "online",
  "timestamp": "YYYY-MM-DD HH:MM:SS",
  "last_measure_time": "YYYY-MM-DD HH:MM:SS|unknown"
}
```

### 6.2 周期上报（Data）

* **Topic**：`post_topic`
* **Payload**（字段随模式有所不同）：

```json
{
  "data": [
    {"key": "<keyTempIn>",  "value": 39.6, "measured_time": "YYYY-MM-DD HH:MM:SS"},
    {"key": "<keyTempOut0>","value": 41.2, "measured_time": "YYYY-MM-DD HH:MM:SS"},
    ...
  ],
  "info": {
    "mode": "setpoint|ncurve",
    "setpoint": 45.0,         // setpoint模式才有
    "set_hyst": 0.8,          // setpoint模式才有
    "tank_temp": 55.4|null,
    "tank_over": false,
    "tank_in_delta": 15.8|null,
    "tank_out_delta": 12.3|null,
    "msg": "[简要决策信息|含Δ_on/Δ_off/boost等]",
    "heat": true|false,
    "pump": true|false,
    "aeration": true|false
  }
}
```

---

## 7. 远程控制与配置（Command）

### 7.1 命令包基本结构

* **订阅Topic**：`response_topic`
* **通用格式**：

```json
{
  "device": "O5DGVbateN",
  "commands": [
    {
      "command": "aeration|heater|pump|config_update",
      "action": "on|off",          // 对于config_update可省略
      "duration": 600000,          // ms，可选；=0表示不自动关闭
      "schedule": "YYYY-MM-DD HH:MM:SS", // 可选，延时执行（本地时区）
      "config": { ... }            // 仅config_update时存在
    }
  ]
}
```

> 队列是**非阻塞**的；`duration>0` 会自动插入一个“定时关闭”命令；支持 `schedule` 定点执行。

### 7.2 动作命令（手动软锁）

* **手动曝气**（5分钟）：

```json
{
  "device": "O5DGVbateN",
  "commands": [{ "command": "aeration", "action": "on", "duration": 300000 }]
}
```

* **手动开加热**（10分钟）：

```json
{
  "device": "O5DGVbateN",
  "commands": [{ "command": "heater", "action": "on", "duration": 600000 }]
}
```

* **手动仅泵**（3分钟）：

```json
{
  "device": "O5DGVbateN",
  "commands": [{ "command": "pump", "action": "on", "duration": 180000 }]
}
```

> 软锁有效期间：自动逻辑不改变对应执行器状态（安全硬保护仍覆盖；互斥策略仍生效）。

### 7.3 远程配置（config_update）

* **切换到 Setpoint 模式（目标48℃，回差0.7℃）**：

```json
{
  "device": "O5DGVbateN",
  "commands": [
    {
      "command": "config_update",
      "config": {
        "bath_setpoint": { "enabled": true, "target": 48.0, "hyst": 0.7 }
      }
    }
  ]
}
```

* **切回 n-curve 模式**：

```json
{
  "device": "O5DGVbateN",
  "commands": [
    {
      "command": "config_update",
      "config": { "bath_setpoint": { "enabled": false } }
    }
  ]
}
```

* **更新曝气策略**：

```json
{
  "device": "O5DGVbateN",
  "commands": [
    {
      "command": "config_update",
      "config": {
        "aeration_timer": { "enabled": true, "interval": 600000, "duration": 120000 }
      }
    }
  ]
}
```

> 设备收到 `config_update` → 合并更新 → 写 `/config.json` → **自动重启** → 新配置生效。

---

## 8. 安全与互斥规则（强制）

1. **外浴硬保护**：`t_out_med ≥ temp_limitout_max` → 立即**关加热+关泵**
2. **Tank 上限**：`tank ≥ tank_temp_max` 或 Tank 无效 → **停热**
3. **互斥**：加热与泵绝不同时运行（开一个前先关另一个）
4. **最小开/停时间**：未满足 `heater_guard` 限制时抑制瞬时抖动
5. **软锁优先**：手动命令持续期内，自动逻辑不改写该执行器状态（除非硬保护）

---

## 9. 存储与相位恢复

* **NVS**：记录“上次测量时间”“上次曝气时间”，**在重启后恢复相位**（避免周期打断）
* **SPIFFS**：持久化 `/config.json`
* **学习补偿状态**：`gPumpDeltaBoost` 运行期内自调，重启后从0开始（如需持久化可扩展NVS）

---

## 10. 日志与诊断

* 串口输出：关键状态、阈值、决策理由、硬保护触发信息
* 上报 `info.msg`：包含模式/决策摘要、`Δ_on/Δ_off/boost`、`t_in/t_out_med/diff`
* 常见告警：

  * `[SAFETY] 外部温度 … ≥ …，强制冷却（关加热+关泵）`
  * `Tank 温度无效或过高，强制关闭加热`
  * `抑制开热：未到最小关断间隔`

---

## 11. 常见问题（FAQ）

**Q1：Setpoint 与 n-curve 有何选择建议？**

* 需要**固定外浴温度**以配合上位实验流程 → 选 **Setpoint**；
* 需要**跟随堆体内温动态补热**、更强调能耗与堆体响应 → 选 **n-curve**。

**Q2：为什么“仅泵助热”有时进不去？**

* 可能 `tank - t_out_med < Δ_on`（水箱还不够热）或“学习补偿”抬高了Δ_on（之前仅泵效果差）。
* 可提升 `tank` 温度（开启加热预热）或调大 `pump_learning.progress_min` 以放宽“有效升温”的判定。

**Q3：频繁开关怎么办？**

* 通过 `heater_guard.min_on_ms/min_off_ms` 增大最小开停时间；Setpoint 模式下适当加大 `hyst`。

**Q4：外浴温度顶到上限了？**

* 检查 `temp_limitout_max`，若实验允许可适当调高（注意安全）；Setpoint 的 `target` 会被夹紧到此上限之下。

---

## 12. 运维建议

* **首次部署**：建议先本地写好 `/config.json` 再上电；或最小连接后用 `config_update` 远程下发完整配置。
* **远程控制**：尽量用 **Setpoint** 执行“外浴控温策略替换”，避免频繁调整 n-curve 系数。
* **能耗优化**：关注上报 `boost` 与 `tank_out_delta`，适当调整 `pump_*` 与 `heater_guard`，形成**更多“仅泵助热”**并保持有效升温。
* **安全边界**：`tank_temp_max` 与 `temp_limitout_max` 切勿设置过高；必要时增加硬件级温控/熔断器。

---

## 13. 版本/兼容性

* 若旧配置中无 `bath_setpoint`，将自动使用默认值（关闭 setpoint，保留 n-curve 行为）。
* 本手册对应的源码已在 `config_manager.h/.cpp` 与 `main.cpp` 全面打通。

---
附config.json 参数说明总表

| 模块          | 字段路径                          | 类型            | 说明                      | 示例值                                 | 默认值      | 单位    | 可远程修改 |
| :---------- | :---------------------------- | :------------ | :---------------------- | :---------------------------------- | :------- | :---- | :---- |
| **WiFi 网络** | `wifi.ssid`                   | String        | Wi-Fi 网络名称（SSID）        | `Compostlab`                        | —        | —     | ✅     |
|             | `wifi.password`               | String        | Wi-Fi 密码                | `znxk8888`                          | —        | —     | ✅     |
| **MQTT 通信** | `mqtt.server`                 | String        | MQTT 服务器地址              | `118.25.108.254`                    | —        | —     | ✅     |
|             | `mqtt.port`                   | UInt16        | MQTT 端口号                | `1883`                              | `1883`   | —     | ✅     |
|             | `mqtt.user`                   | String        | 登录用户名                   | `equipment`                         | 空        | —     | ✅     |
|             | `mqtt.pass`                   | String        | 登录密码                    | `ZNXK8888`                          | 空        | —     | ✅     |
|             | `mqtt.clientId`               | String        | 客户端 ID（设备唯一标识）          | `cp500_006`                         | `cp500`  | —     | ✅     |
|             | `mqtt.post_topic`             | String        | 上报数据主题                  | `compostlab/O5DGVbateN/post/data`   | —        | —     | ✅     |
|             | `mqtt.response_topic`         | String        | 指令接收主题                  | `compostlab/O5DGVbateN/response`    | —        | —     | ✅     |
| **数据字段映射**  | `keys.temp_in`                | String        | 堆体内部温度字段 Key            | `mqEGyxQVU56KPye`                   | —        | —     | ✅     |
|             | `keys.temp_out[]`             | Array<String> | 外浴多探头字段 Key 列表          | `["wBsh...", "dD8H...", "E9Pv..."]` | —        | —     | ✅     |
| **上传与时间同步** | `post_interval`               | UInt32        | 采集-控制-上报周期              | `60000`                             | `60000`  | `ms`  | ✅     |
|             | `ntp_host[]`                  | Array<String> | NTP 时间服务器列表             | `ntp.aliyun.com` 等                  | 内置3个     | —     | ✅     |
| **温度上下限**   | `temp_limitout_max`           | UInt32        | 外浴最高温限制（超限停热）           | `65`                                | `75`     | `℃`   | ✅     |
|             | `temp_limitin_max`            | UInt32        | 堆体最高温（用于曲线归一化）          | `70`                                | `70`     | `℃`   | ✅     |
|             | `temp_limitout_min`           | UInt32        | 外浴最低温（低于可强制加热）          | `25`                                | `25`     | `℃`   | ✅     |
|             | `temp_limitin_min`            | UInt32        | 堆体最低温（低于不触发阈值）          | `25`                                | `25`     | `℃`   | ✅     |
|             | `temp_maxdif`                 | UInt32        | 堆体与外浴的最大差阈（n-curve 基准）  | `5`                                 | `5`      | `℃`   | ✅     |
| **设备信息**    | `equipment_key`               | String        | 设备唯一标识（与上位系统对应）         | `O5DGVbateN`                        | —        | —     | ✅     |
| **外浴定置控温**  | `bath_setpoint.enabled`       | Bool          | 是否启用外浴定置控温模式            | `false`                             | `false`  | —     | ✅     |
|             | `bath_setpoint.target`        | Float         | 外浴目标温度（中位温）             | `45.0`                              | `45.0`   | `℃`   | ✅     |
|             | `bath_setpoint.hyst`          | Float         | 控温回差（防止抖动）              | `0.8`                               | `0.8`    | `℃`   | ✅     |
| **曝气定时控制**  | `aeration_timer.enabled`      | Bool          | 是否启用定时曝气                | `true`                              | `false`  | —     | ✅     |
|             | `aeration_timer.interval`     | UInt32        | 曝气间隔周期                  | `0`                                 | `600000` | `ms`  | ✅     |
|             | `aeration_timer.duration`     | UInt32        | 每次曝气持续时长                | `60000000000`                       | `300000` | `ms`  | ✅     |
| **安全保护**    | `safety.tank_temp_max`        | Float         | 水箱安全温度上限（超限停热）          | `90.0`                              | `90.0`   | `℃`   | ✅     |
| **加热防抖参数**  | `heater_guard.min_on_ms`      | UInt32        | 加热器最小开机时长               | `30000`                             | `30000`  | `ms`  | ✅     |
|             | `heater_guard.min_off_ms`     | UInt32        | 加热器最小关机时长               | `30000`                             | `30000`  | `ms`  | ✅     |
| **水泵自适应控制** | `pump_adaptive.delta_on_min`  | Float         | Δ_on 低温下限（堆体低温时要求的热差）   | `6.0`                               | `6.0`    | `℃`   | ✅     |
|             | `pump_adaptive.delta_on_max`  | Float         | Δ_on 高温上限（堆体高温时的热差要求）   | `30.0`                              | `25.0`   | `℃`   | ✅     |
|             | `pump_adaptive.hyst_nom`      | Float         | 名义回差（用于推算 Δ_off）        | `3.0`                               | `3.0`    | `℃`   | ✅     |
|             | `pump_adaptive.ncurve_gamma`  | Float         | n-curve 曲线指数（决定Δ变化曲线形状） | `1.3`                               | `1.3`    | —     | ✅     |
| **水泵学习补偿**  | `pump_learning.step_up`       | Float         | 升温无效时 Δ_on 增加步长         | `0.5`                               | `0.5`    | `℃/次` | ✅     |
|             | `pump_learning.step_down`     | Float         | 升温有效或非仅泵时阈值回落步长         | `0.2`                               | `0.2`    | `℃/次` | ✅     |
|             | `pump_learning.max`           | Float         | 学习补偿最大值                 | `10.0`                              | `8.0`    | `℃`   | ✅     |
|             | `pump_learning.progress_min`  | Float         | 判定“仅泵有效升温”的最小升温阈值       | `0.05`                              | `0.05`   | `℃`   | ✅     |
| **差阈曲线特性**  | `curves.in_diff_ncurve_gamma` | Float         | 堆体温与差阈关系指数              | `2.0`                               | `2.0`    | —     | ✅     |
