// main.cpp
// 负责设备启动、配置同步、MQTT 指令处理、定时采样与数据上报。
// 当前传感器组合:
// - MH-Z16: CO2
// - ZCE04B: CO / H2S / O2 / CH4
// - SHT30: AirTemp / AirHumidity

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"
#include "data_buffer.h"

// ======================= 持久化 =======================
// NVS 用来保存“上一轮巡检进行到哪里了”，这样设备意外重启后还能续跑。
Preferences preferences;
static const char* NVS_NAMESPACE = "my-nvs";
// 上一轮巡检开始时间，用于按 read_interval 恢复节奏。
static const char* NVS_KEY_LAST_MEAS = "lastMeas";
// 续跑状态：是否存在被中断的巡检。
static const char* NVS_KEY_RESUME_ACTIVE = "resActive";
// 续跑状态：被中断时停在第几个采样点。
static const char* NVS_KEY_RESUME_POINT = "resPoint";
// 续跑状态：被中断时停在该点的哪个阶段。
static const char* NVS_KEY_RESUME_PHASE = "resPhase";
// 本轮里最近一次已完成的点位标记，用于重启后避免重复上报。
static const char* NVS_KEY_DONE_CYCLE = "doneCycle";
static const char* NVS_KEY_DONE_POINT = "donePoint";
// 启动后距离第一轮巡检还需要等待多久。
static unsigned long g_initialMeasureDelayMs = 0;
// 是否需要从中断点恢复一轮未完成的巡检。
static bool g_resumePending = false;
// 恢复时从哪个点位继续。
static size_t g_resumePointIndex = 0;

enum class ResumePhase : uint8_t {
  // 没有恢复任务。
  Idle = 0,
  // 恢复到“点位泵抽气 + 检测”阶段。
  PointPump = 1,
  // 恢复到“吹扫泵清空气路”阶段。
  PurgePump = 2,
};

// 当前待恢复任务所在的阶段。
static ResumePhase g_resumePhase = ResumePhase::Idle;

// 串口传感器与 I2C 温湿度传感器引脚定义。
static constexpr int PIN_MHZ16_RX = 18;
static constexpr int PIN_MHZ16_TX = 19;
static constexpr int PIN_ZCE04B_RX = 16;
static constexpr int PIN_ZCE04B_TX = 17;
static constexpr uint8_t PIN_SHT30_SDA = 21;
static constexpr uint8_t PIN_SHT30_SCL = 22;

// 6 个采样点 + 1 个吹扫泵，共 7 路继电器输出。
static constexpr size_t POINT_COUNT = AppConfig::kPointCount;
static constexpr size_t PURGE_PUMP_INDEX = POINT_COUNT;
static constexpr size_t TOTAL_PUMP_COUNT = POINT_COUNT + 1;
static const uint8_t PUMP_PINS[TOTAL_PUMP_COUNT] = { 13, 14, 25, 26, 27, 32, 33 };

// MQTT 命令队列和采样流程用到的运行参数。
static constexpr int MAX_PENDING_COMMANDS = 50;
// 长时间泵控命令分片执行，避免一次 delay 太久完全不让出 CPU。
static constexpr unsigned long COMMAND_SLICE_MS = 5000;
// 主循环尝试上传离线缓存数据的周期。
static constexpr unsigned long CACHE_UPLOAD_INTERVAL_MS = 30000;

// 采样流程默认参数：
// sample_time 现在表示“取样抽气时长”，只负责把有限气体抽到传感器腔体。
static constexpr unsigned long DEFAULT_SAMPLE_INTAKE_MS = 10000;
// 停泵后的静态检测窗口，在这个窗口里尽量少消耗气体、连续观察趋势。
static constexpr unsigned long DEFAULT_STATIC_MEASURE_WINDOW_MS = 30000;
// 静态检测阶段的固定采样间隔。
static constexpr unsigned long DEFAULT_STATIC_SAMPLE_INTERVAL_MS = 5000;
// 停泵后的最小观察期。先观察几次，再开始判稳，避免刚停泵就立刻下结论。
static constexpr unsigned long DEFAULT_STATIC_STABILIZATION_MS = 10000;

// 判稳窗口至少要有这么多个有效点才开始算趋势。
static constexpr size_t STABILITY_MIN_VALID_SAMPLES = 3;
// CO2 / O2 判稳阈值，单位是“相对变化率 %/分钟”。
static constexpr float CO2_STABLE_SLOPE_THRESHOLD_PERCENT_PER_MIN = 5.0f;
static constexpr float O2_STABLE_SLOPE_THRESHOLD_PERCENT_PER_MIN = 1.5f;
// 相对变化率计算时的参考下限，避免数值过小导致百分比失真。
static constexpr float CO2_STABILITY_REFERENCE_MIN_PPM = 800.0f;
static constexpr float O2_STABILITY_REFERENCE_MIN_PERCENT = 5.0f;

// 采样策略说明：
// 1. 先短时间抽气 sample_time，把有限气体送到传感器腔体。
// 2. 然后停泵，在静态窗口里连续读数据，尽量减少继续耗气。
// 3. 先经过一个最小观察期，避免刚停泵时残余流动影响判稳。
// 4. 之后同时检查 CO2 和 O2 的相对变化率，双通道都足够平稳才算进入稳态。
// 5. 稳态样本最终用抗异常值统计，而不是简单求平均。

// ======================= 命令队列 =======================
struct PendingCommand {
  String cmd;
  String action;
  unsigned long duration;
  time_t targetTime;
};
std::vector<PendingCommand> pendingCommands;
static SemaphoreHandle_t g_cmdMutex = nullptr;
static volatile bool g_pendingRestart = false;
static unsigned long g_restartAtMs = 0;
// 自动巡检进行中时，禁止远程手动泵控，避免打乱当前气路。
static volatile bool g_measurementInProgress = false;

// =====================================================
// 工具：从 JsonVariant 读 String（空则返回 defaultVal）
// =====================================================
static String readStr(JsonVariant v, const char* defaultVal = "") {
  if (v.is<const char*>()) return String(v.as<const char*>());
  if (v.is<String>())      return v.as<String>();
  return String(defaultVal);
}

static const char* qualityOf(float value) {
  return value < 0 ? "ERR" : "OK";
}

static void appendChannel(
  String& payload,
  bool& firstChannel,
  const char* code,
  float value,
  int decimals,
  const char* unit,
  const char* qualityOverride = nullptr) {
  if (!firstChannel) {
    payload += ",";
  }
  firstChannel = false;

  payload += "{";
  payload += "\"code\":\"" + String(code) + "\",";
  payload += "\"value\":" + String(value, decimals) + ",";
  payload += "\"unit\":\"" + String(unit) + "\",";
  payload += "\"quality\":\"" + String(qualityOverride ? qualityOverride : qualityOf(value)) + "\"";
  payload += "}";
}

static void runPumpForDuration(size_t pumpIndex, unsigned long durationMs) {
  if (durationMs == 0) {
    pumpOff(pumpIndex);
    return;
  }
  pumpOn(pumpIndex);

  unsigned long remaining = durationMs;
  while (remaining > 0) {
    unsigned long chunk = min(remaining, COMMAND_SLICE_MS);
    delay(chunk);
    remaining -= chunk;
    yield();
  }

  pumpOff(pumpIndex);
}

// =====================================================
// 工具：解析 schedule（"YYYY-MM-DD HH:MM:SS"），失败则返回 now
// =====================================================
static time_t parseScheduleOrNow(const String& schedule) {
  time_t target = time(nullptr);
  if (schedule.length() == 0) return target;

  struct tm tm_s {};
  if (strptime(schedule.c_str(), "%Y-%m-%d %H:%M:%S", &tm_s)) {
    target = mktime(&tm_s);
  }
  return target;
}

static int pumpIndexFromCommand(const String& cmd) {
  if (cmd == "purge") {
    return (int)PURGE_PUMP_INDEX;
  }
  if (cmd.startsWith("point")) {
    int pointNo = cmd.substring(5).toInt();
    if (pointNo >= 1 && pointNo <= (int)POINT_COUNT) {
        return pointNo - 1;
      }
    }
  return -1;
}

static unsigned long effectiveSampleIntakeMs() {
  return appConfig.sampleTime > 0 ? appConfig.sampleTime : DEFAULT_SAMPLE_INTAKE_MS;
}

static unsigned long effectiveStaticMeasureWindowMs() {
  return appConfig.staticMeasureTime > 0 ? appConfig.staticMeasureTime : DEFAULT_STATIC_MEASURE_WINDOW_MS;
}

static unsigned long effectiveSampleIntervalMs() {
  return DEFAULT_STATIC_SAMPLE_INTERVAL_MS;
}

static unsigned long effectiveSampleStabilizationMs() {
  unsigned long windowMs = effectiveStaticMeasureWindowMs();
  unsigned long stabilizationMs = DEFAULT_STATIC_STABILIZATION_MS;
  if (stabilizationMs >= windowMs) {
    return windowMs > 1000 ? (windowMs - 1000) : 0;
  }
  return stabilizationMs;
}

static size_t estimatedSampleCount() {
  return (size_t)((effectiveStaticMeasureWindowMs() - 1) / effectiveSampleIntervalMs()) + 1;
}

struct RobustSeries {
  std::vector<float> values;

  void reserve(size_t n) {
    values.reserve(n);
  }

  void add(float value) {
    if (value < 0) {
      return;
    }
    values.push_back(value);
  }

  size_t count() const {
    return values.size();
  }

  float robustAverageOr(float fallback = -1.0f) const {
    // 小样本时偏向中位数，大样本时去掉头尾异常值后再求均值，
    // 这样比直接平均更抗现场偶发尖峰。
    if (values.empty()) {
      return fallback;
    }

    std::vector<float> sorted = values;
    std::sort(sorted.begin(), sorted.end());

    const size_t n = sorted.size();
    if (n == 1) {
      return sorted[0];
    }
    if (n == 2) {
      return (sorted[0] + sorted[1]) / 2.0f;
    }
    if (n <= 4) {
      return sorted[n / 2];
    }

    size_t trim = 1;
    if (n >= 10) {
      trim = n / 5;
    }

    if (trim * 2 >= n) {
      trim = 0;
    }

    float sum = 0.0f;
    size_t used = 0;
    for (size_t i = trim; i < n - trim; ++i) {
      sum += sorted[i];
      used++;
    }

    return used > 0 ? (sum / (float)used) : fallback;
  }
};

struct StableAverages {
  RobustSeries co2ppm;
  RobustSeries co;
  RobustSeries h2s;
  RobustSeries o2;
  RobustSeries ch4;
  RobustSeries airTemp;
  RobustSeries airHumidity;

  void reserve(size_t n) {
    co2ppm.reserve(n);
    co.reserve(n);
    h2s.reserve(n);
    o2.reserve(n);
    ch4.reserve(n);
    airTemp.reserve(n);
    airHumidity.reserve(n);
  }
};

struct TimedSamplePoint {
  unsigned long elapsedMs;
  float value;
};

static bool isTrendStable(const std::vector<TimedSamplePoint>& history, float slopeThresholdPercentPerMin, float minReferenceValue) {
  // 用最近几个有效点估算“相对变化率/分钟”。
  // 只有变化率足够小，才认为该通道已经趋稳。
  if (history.size() < STABILITY_MIN_VALID_SAMPLES) {
    return false;
  }

  const size_t startIndex = history.size() - STABILITY_MIN_VALID_SAMPLES;
  const TimedSamplePoint& first = history[startIndex];
  const TimedSamplePoint& last = history.back();
  if (last.elapsedMs <= first.elapsedMs) {
    return false;
  }

  const float deltaMinutes = (float)(last.elapsedMs - first.elapsedMs) / 60000.0f;
  if (deltaMinutes <= 0.0f) {
    return false;
  }

  const float deltaValue = last.value - first.value;
  const float referenceValue = max(minReferenceValue, (fabsf(first.value) + fabsf(last.value)) / 2.0f);
  const float slopePercentPerMin = (deltaValue / referenceValue) * 100.0f / deltaMinutes;
  return fabsf(slopePercentPerMin) <= slopeThresholdPercentPerMin;
}

static unsigned long estimatedMinCycleMs() {
  return (unsigned long)POINT_COUNT * (effectiveSampleIntakeMs() + effectiveStaticMeasureWindowMs() + appConfig.purgePumpTime);
}

static void logCycleBudget(const char* prefix) {
  unsigned long estimatedMs = estimatedMinCycleMs();
  long remainingMs = (long)appConfig.readInterval - (long)estimatedMs;
  Serial.printf("%s Estimated minimum cycle=%lu ms, read_interval=%lu ms, remaining=%ld ms\n",
    prefix,
    estimatedMs,
    appConfig.readInterval,
    remainingMs);
  if (remainingMs <= 0) {
    Serial.println("[Measure] Warning: read_interval is not larger than the estimated cycle duration");
  }
}

static bool beginPrefs() {
  if (!preferences.begin(NVS_NAMESPACE, false)) {
    Serial.println("[State] Failed to open NVS");
    return false;
  }
  return true;
}

static void saveResumeState(bool active, size_t pointIndex, ResumePhase phase) {
  if (!beginPrefs()) {
    return;
  }
  preferences.putBool(NVS_KEY_RESUME_ACTIVE, active);
  preferences.putUInt(NVS_KEY_RESUME_POINT, (unsigned int)pointIndex);
  preferences.putUChar(NVS_KEY_RESUME_PHASE, (uint8_t)phase);
  preferences.end();
  Serial.printf("[State] Saved resume state: active=%s, point=%u, phase=%u\n",
    active ? "true" : "false",
    (unsigned)(pointIndex + 1),
    (unsigned)phase);
}

static void clearResumeState() {
  saveResumeState(false, 0, ResumePhase::Idle);
}

static void savePointCompletion(time_t cycleStartEpoch, size_t pointIndex) {
  if (!beginPrefs()) {
    return;
  }
  preferences.putULong(NVS_KEY_DONE_CYCLE, (unsigned long)cycleStartEpoch);
  preferences.putUInt(NVS_KEY_DONE_POINT, (unsigned int)pointIndex);
  preferences.end();
  Serial.printf("[State] Saved point completion: cycle=%lu, point=%u\n",
    (unsigned long)cycleStartEpoch,
    (unsigned)(pointIndex + 1));
}

static void clearPointCompletion() {
  if (!beginPrefs()) {
    return;
  }
  preferences.putULong(NVS_KEY_DONE_CYCLE, 0);
  preferences.putUInt(NVS_KEY_DONE_POINT, 0);
  preferences.end();
  Serial.println("[State] Cleared point completion marker");
}

// =====================================================
// 生成"完整当前配置"的 JSON（用于上线/回执）
// 格式：{ "wifi": {...}, "mqtt": {...}, "ntp_servers": [...], "sample_time": ..., "static_measure_time": ..., "purge_pump_time": ..., "read_interval": ... }
// =====================================================
static void fillConfigJson(JsonObject cfg) {
  // WiFi
  JsonObject wifi = cfg["wifi"].to<JsonObject>();
  wifi["ssid"] = appConfig.wifiSSID;
  wifi["password"] = appConfig.wifiPass;

  // MQTT
  JsonObject mqtt = cfg["mqtt"].to<JsonObject>();
  mqtt["server"] = appConfig.mqttServer;
  mqtt["port"] = appConfig.mqttPort;
  mqtt["user"] = appConfig.mqttUser;
  mqtt["pass"] = appConfig.mqttPass;
  mqtt["device_code"] = appConfig.deviceCode;
  JsonArray pointCodes = mqtt["point_device_codes"].to<JsonArray>();
  for (auto& code : appConfig.pointDeviceCodes) pointCodes.add(code);

  // NTP servers
  JsonArray ntps = cfg["ntp_servers"].to<JsonArray>();
  for (auto& s : appConfig.ntpServers) ntps.add(s);

  // 控制参数
  cfg["sample_time"] = appConfig.sampleTime;
  cfg["static_measure_time"] = appConfig.staticMeasureTime;
  cfg["purge_pump_time"] = appConfig.purgePumpTime;
  cfg["read_interval"] = appConfig.readInterval;
}

// =====================================================
// 发布上线消息（带完整配置）
// topic: compostlab/v2/{device_code}/register
// =====================================================
static void publishOnlineWithConfig() {
  Serial.println("[Register] Preparing registration payload...");

  JsonDocument doc;
  doc["schema_version"] = 2;

  // 使用公网 IP 地址
  String ipAddress = getPublicIP();
  doc["ip_address"] = ipAddress;

  String timestamp = getTimeString();
  doc["timestamp"] = timestamp;
  Serial.printf("[Register] Timestamp: %s\n", timestamp.c_str());
  Serial.printf("[Register] IP Address: %s\n", ipAddress.c_str());

  JsonObject cfg = doc["config"].to<JsonObject>();
  fillConfigJson(cfg);

  String out;
  serializeJson(doc, out);
  Serial.printf("[Register] Payload size: %d bytes\n", out.length());

  // 使用注册 topic: compostlab/v2/{device_code}/register
  String registerTopic = "compostlab/v2/" + appConfig.deviceCode + "/register";
  Serial.printf("[Register] Topic: %s\n", registerTopic.c_str());

  bool result = publishData(registerTopic, out, 10000);
  if (result) {
    Serial.println("[Register] Registration message published successfully");
  }
  else {
    Serial.println("[Register] Failed to publish registration message");
  }
}

// =====================================================
// 远程配置更新：只更新指令里出现的字段，其它保持原状
// 与 config_manager.cpp 存储结构保持一致（/config.json）
// =====================================================
static bool updateAppConfigFromJson(JsonObject cfg) {

  // -------- sample_time / static_measure_time / purge_pump_time / read_interval --------
  // 支持：sample_time, static_measure_time, purge_pump_time, read_interval
  if (cfg["sample_time"].is<uint32_t>()) {
    appConfig.sampleTime = cfg["sample_time"].as<uint32_t>();
    Serial.printf("[CFG] sample_time = %u\n", (unsigned)appConfig.sampleTime);
  }

  if (cfg["static_measure_time"].is<uint32_t>()) {
    appConfig.staticMeasureTime = cfg["static_measure_time"].as<uint32_t>();
    Serial.printf("[CFG] static_measure_time = %u\n", (unsigned)appConfig.staticMeasureTime);
  }

  if (cfg["purge_pump_time"].is<uint32_t>()) {
    appConfig.purgePumpTime = cfg["purge_pump_time"].as<uint32_t>();
    Serial.printf("[CFG] purge_pump_time = %u\n", (unsigned)appConfig.purgePumpTime);
  }

  if (cfg["read_interval"].is<uint32_t>()) {
    appConfig.readInterval = cfg["read_interval"].as<uint32_t>();
    Serial.printf("[CFG] read_interval = %u\n", (unsigned)appConfig.readInterval);
  }

  // -------- WiFi --------
  if (cfg["wifi"].is<JsonObject>()) {
    JsonObject wifi = cfg["wifi"].as<JsonObject>();
    if (wifi["ssid"].is<String>() || wifi["ssid"].is<const char*>())
      appConfig.wifiSSID = readStr(wifi["ssid"]);
    if (wifi["password"].is<String>() || wifi["password"].is<const char*>())
      appConfig.wifiPass = readStr(wifi["password"]);
  }

  // -------- MQTT --------
  if (cfg["mqtt"].is<JsonObject>()) {
    JsonObject mqtt = cfg["mqtt"].as<JsonObject>();
    if (mqtt["server"].is<String>() || mqtt["server"].is<const char*>())
      appConfig.mqttServer = readStr(mqtt["server"]);
    if (mqtt["port"].is<uint16_t>() || mqtt["port"].is<uint32_t>())
      appConfig.mqttPort = (uint16_t)mqtt["port"].as<uint32_t>();

    if (mqtt["user"].is<String>() || mqtt["user"].is<const char*>())
      appConfig.mqttUser = readStr(mqtt["user"]);
    if (mqtt["pass"].is<String>() || mqtt["pass"].is<const char*>())
      appConfig.mqttPass = readStr(mqtt["pass"]);

    if (mqtt["clientId"].is<String>() || mqtt["clientId"].is<const char*>())
      appConfig.mqttClientId = readStr(mqtt["clientId"]);

    if (mqtt["device_code"].is<String>() || mqtt["device_code"].is<const char*>())
      appConfig.deviceCode = readStr(mqtt["device_code"]);

    if (mqtt["point_device_codes"].is<JsonArray>()) {
      JsonArray pointCodes = mqtt["point_device_codes"].as<JsonArray>();
      appConfig.pointDeviceCodes.clear();
      for (JsonVariant v : pointCodes) {
        String code = readStr(v, "");
        if (code.length() > 0) {
          appConfig.pointDeviceCodes.push_back(code);
        }
      }
      while (appConfig.pointDeviceCodes.size() < POINT_COUNT) {
        appConfig.pointDeviceCodes.push_back(appConfig.deviceCode + "-P" + String(appConfig.pointDeviceCodes.size() + 1));
      }
    }

    // post_topic 和 response_topic 现在自动根据 device_code 生成，忽略配置中的字段
  }

  // -------- NTP servers --------
  // 支持：ntp_servers: ["a","b","c"]
  if (cfg["ntp_servers"].is<JsonArray>()) {
    JsonArray arr = cfg["ntp_servers"].as<JsonArray>();
    appConfig.ntpServers.clear();
    for (JsonVariant v : arr) {
      String s = readStr(v, "");
      if (s.length() > 0) appConfig.ntpServers.push_back(s);
    }
    Serial.printf("[CFG] ntp_servers size = %u\n", (unsigned)appConfig.ntpServers.size());
  }

  // -------- device_code 兼容旧配置格式 --------
  if (cfg["equipment_key"].is<const char*>() || cfg["equipment_key"].is<String>()) {
    appConfig.deviceCode = readStr(cfg["equipment_key"]);
  }

  while (appConfig.pointDeviceCodes.size() < POINT_COUNT) {
    appConfig.pointDeviceCodes.push_back(appConfig.deviceCode + "-P" + String(appConfig.pointDeviceCodes.size() + 1));
  }

  return true;
}

// =====================================================
// 执行控制命令（普通控制：pump/restart）
// 注意：对于长时间的操作，会阻塞任务执行，建议 duration 不要太大
// =====================================================
static void executeCommand(const PendingCommand& pcmd) {
  Serial.printf("[CMD] Executing command: %s %s (duration=%lu ms, targetEpoch=%lu)\n",
    pcmd.cmd.c_str(), pcmd.action.c_str(), pcmd.duration, (unsigned long)pcmd.targetTime);

  // ---- 重启 ----
  if (pcmd.cmd == "restart") {
    Serial.println("[CMD] Remote restart requested");
    delay(300);
    ESP.restart();
    return;
  }

  int pumpIndex = pumpIndexFromCommand(pcmd.cmd);
  if (pumpIndex >= 0) {
    if (g_measurementInProgress) {
      Serial.printf("[CMD] Ignored manual pump command during measurement cycle: %s %s\n",
        pcmd.cmd.c_str(), pcmd.action.c_str());
      return;
    }
    Serial.printf("[CMD] Pump command mapped to index=%d\n", pumpIndex);
    if (pcmd.action == "on") {
      runPumpForDuration((size_t)pumpIndex, pcmd.duration);
    }
    else {
      pumpOff((size_t)pumpIndex);
    }
    return;
  }

  Serial.println("[CMD] Unknown command: " + pcmd.cmd);
}

// =====================================================
// MQTT 回调：统一解析 commands
//  - config_update/update_config：立即更新->保存->重启
//  - restart/pump：进入队列，支持 schedule/duration
// =====================================================
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] Message received on topic=%s, payloadLength=%u\n", topic, length);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("[MQTT] JSON parse failed: ");
    Serial.println(err.c_str());
    return;
  }

  String device = readStr(doc["device"], "");
  if (device != appConfig.deviceCode) {
    Serial.printf("[MQTT] Device mismatch, ignoring message (incoming=%s, local=%s)\n",
      device.c_str(), appConfig.deviceCode.c_str());
    return;
  }

  if (!doc["commands"].is<JsonArray>()) {
    Serial.println("[MQTT] Missing or invalid commands array");
    return;
  }

  JsonArray cmds = doc["commands"].as<JsonArray>();
  Serial.printf("[MQTT] Parsed %u command(s)\n", (unsigned)cmds.size());

  for (JsonVariant v : cmds) {
    if (!v.is<JsonObject>()) continue;

    JsonObject obj = v.as<JsonObject>();
    String cmd = readStr(obj["command"], "");

    // 兼容两种命令名
    bool isCfgUpdate = (cmd == "config_update" || cmd == "update_config");
    if (isCfgUpdate) {
      if (!obj["config"].is<JsonObject>()) {
        Serial.println("[CFG] Missing or invalid config object");
        continue;
      }

      JsonObject cfg = obj["config"].as<JsonObject>();
      Serial.println("[CFG] Applying remote configuration update...");

      if (!updateAppConfigFromJson(cfg)) {
        Serial.println("[CFG] Failed to parse remote configuration");
        continue;
      }

      if (!saveConfigToSPIFFS("/config.json")) {
        Serial.println("[CFG] Failed to save configuration to SPIFFS");
        continue;
      }

      Serial.println("[CFG] Configuration saved, restart scheduled in 3 seconds");

      // 仅设置重启标记，避免在回调中阻塞
      g_pendingRestart = true;
      g_restartAtMs = millis() + 3000;
      return;
    }

    // 普通控制命令：action/duration/schedule
    String action = readStr(obj["action"], "");
    unsigned long dur = 0;
    if (obj["duration"].is<uint32_t>() || obj["duration"].is<uint64_t>())
      dur = obj["duration"].as<uint32_t>();

    String schedule = readStr(obj["schedule"], "");
    time_t target = parseScheduleOrNow(schedule);
    Serial.printf("[CMD] Queue request: command=%s, action=%s, duration=%lu ms, schedule=%s, targetEpoch=%lu\n",
      cmd.c_str(),
      action.c_str(),
      dur,
      schedule.length() ? schedule.c_str() : "<now>",
      (unsigned long)target);

    if (g_cmdMutex) {
      xSemaphoreTake(g_cmdMutex, portMAX_DELAY);
    }
    // 检查队列是否已满
    if ((int)pendingCommands.size() >= MAX_PENDING_COMMANDS) {
      Serial.printf("[CMD] Command queue full (%u items), ignoring request\n",
        (unsigned)pendingCommands.size());
      if (g_cmdMutex) {
        xSemaphoreGive(g_cmdMutex);
      }
      continue;
    }
    pendingCommands.push_back({ cmd, action, dur, target });
    Serial.printf("[CMD] Command queued successfully, queueSize=%u\n", (unsigned)pendingCommands.size());
    if (g_cmdMutex) {
      xSemaphoreGive(g_cmdMutex);
    }
  }
}

// =====================================================
// 采样与上传（6 measurement points + 1 purge pump）
// =====================================================
static bool doMeasurementAndSave(size_t startPointIndex = 0, ResumePhase startPhase = ResumePhase::PointPump) {
  const unsigned long sampleIntakeMs = effectiveSampleIntakeMs();
  const unsigned long staticMeasureWindowMs = effectiveStaticMeasureWindowMs();
  const unsigned long sampleStabilizationMs = effectiveSampleStabilizationMs();
  const unsigned long sampleIntervalMs = effectiveSampleIntervalMs();
  const size_t sampleCount = estimatedSampleCount();
  Serial.printf("[Measure] Starting round-robin cycle (readInterval=%lu ms, intake=%lu ms, staticWindow=%lu ms, minObserve=%lu ms, sampleInterval=%lu ms, expectedSamples=%u, co2SlopeThreshold=%.1f %%/min, o2SlopeThreshold=%.2f %%/min, purgePumpTime=%lu ms)\n",
    appConfig.readInterval, sampleIntakeMs, staticMeasureWindowMs, sampleStabilizationMs, sampleIntervalMs, (unsigned)sampleCount, CO2_STABLE_SLOPE_THRESHOLD_PERCENT_PER_MIN, O2_STABLE_SLOPE_THRESHOLD_PERCENT_PER_MIN, appConfig.purgePumpTime);
  logCycleBudget("[Measure]");

  bool cycleOk = true;
  g_measurementInProgress = true;
  time_t cycleStartEpoch = time(nullptr);
  if (startPointIndex == 0 && startPhase == ResumePhase::PointPump) {
    if (beginPrefs()) {
      preferences.putULong(NVS_KEY_LAST_MEAS, (unsigned long)cycleStartEpoch);
      preferences.end();
      Serial.printf("[Measure] Stored cycle start epoch=%lu in NVS\n", (unsigned long)cycleStartEpoch);
    }
  }
  else {
    if (beginPrefs()) {
      cycleStartEpoch = (time_t)preferences.getULong(NVS_KEY_LAST_MEAS, (unsigned long)cycleStartEpoch);
      preferences.end();
      Serial.printf("[Measure] Resuming cycle started at epoch=%lu\n", (unsigned long)cycleStartEpoch);
    }
  }

  clearPointCompletion();

  for (size_t pointIndex = startPointIndex; pointIndex < POINT_COUNT; ++pointIndex) {
    String pointCode = appConfig.pointDeviceCodes[pointIndex];
    if (pointCode.length() == 0) {
      pointCode = appConfig.deviceCode + "-P" + String(pointIndex + 1);
    }

    Serial.printf("[Measure] Point %u/%u using deviceCode=%s, pumpPin=%u\n",
      (unsigned)(pointIndex + 1),
      (unsigned)POINT_COUNT,
      pointCode.c_str(),
      (unsigned)PUMP_PINS[pointIndex]);

    if (!(pointIndex == startPointIndex && startPhase == ResumePhase::PurgePump)) {
      saveResumeState(true, pointIndex, ResumePhase::PointPump);

      allPumpsOff();
      Serial.printf("[Measure] Point pump ON for intake %lu ms to pull limited gas into sensor chamber\n",
        sampleIntakeMs);
      pumpOn(pointIndex);
      delay(sampleIntakeMs);
      pumpOff(pointIndex);
      Serial.printf("[Measure] Point pump OFF, start static measure window %lu ms (minimum observe=%lu ms)\n",
        staticMeasureWindowMs,
        sampleStabilizationMs);

      // stable: 真正通过 CO2+O2 双通道判稳后的样本。
      // fallback: 没判稳时，保留“观察期之后”的样本做兜底统计。
      // 其中 CO2 / O2 会分别按各自的稳定结果标记 quality。
      StableAverages stable;
      stable.reserve(sampleCount);
      StableAverages fallback;
      fallback.reserve(sampleCount);
      std::vector<TimedSamplePoint> co2History;
      co2History.reserve(sampleCount);
      std::vector<TimedSamplePoint> o2History;
      o2History.reserve(sampleCount);
      unsigned long staticStartMs = millis();
      size_t sampleNo = 0;
      bool stableDetected = false;
      bool latestCo2Stable = false;
      bool latestO2Stable = false;
      bool lastEnoughObserveTime = false;
      while ((millis() - staticStartMs) < staticMeasureWindowMs) {
        sampleNo++;
        int co2ppmRaw = readMHZ16();

        ZCE04BGasData gasData{};
        bool gasOk = readZCE04B(gasData);
        float co = gasOk ? gasData.co : -1.0f;
        float h2s = gasOk ? gasData.h2s : -1.0f;
        float o2 = gasOk ? gasData.o2 : -1.0f;
        float ch4 = gasOk ? gasData.ch4 : -1.0f;

        SHT30Data shtData{};
        bool shtOk = readSHT30(shtData);
        float t_air = shtOk ? shtData.temperature : -1.0f;
        float h_air = shtOk ? shtData.humidity : -1.0f;

        unsigned long elapsedMs = millis() - staticStartMs;
        if (co2ppmRaw > 0) {
          co2History.push_back({ elapsedMs, (float)co2ppmRaw });
        }
        if (o2 >= 0.0f) {
          o2History.push_back({ elapsedMs, o2 });
        }

        const bool enoughObserveTime = elapsedMs >= sampleStabilizationMs;
        const bool co2StableNow = enoughObserveTime && isTrendStable(
          co2History,
          CO2_STABLE_SLOPE_THRESHOLD_PERCENT_PER_MIN,
          CO2_STABILITY_REFERENCE_MIN_PPM);
        const bool o2StableNow = enoughObserveTime && isTrendStable(
          o2History,
          O2_STABLE_SLOPE_THRESHOLD_PERCENT_PER_MIN,
          O2_STABILITY_REFERENCE_MIN_PERCENT);
        latestCo2Stable = co2StableNow;
        latestO2Stable = o2StableNow;
        lastEnoughObserveTime = enoughObserveTime;
        const bool trendStableNow = co2StableNow && o2StableNow;
        if (!stableDetected && trendStableNow) {
          stableDetected = true;
          Serial.printf("[Measure] Point %u dual-channel stability accepted at sample %u elapsed=%lu ms\n",
            (unsigned)(pointIndex + 1),
            (unsigned)sampleNo,
            elapsedMs);
        }

        Serial.printf("[Measure] Point %u sample %u elapsed=%lu ms stable=%s observeReady=%s co2Stable=%s o2Stable=%s CO2=%d ppm CO=%.1f H2S=%.1f O2=%.2f CH4=%.1f Temp=%.1f RH=%.1f\n",
          (unsigned)(pointIndex + 1),
          (unsigned)sampleNo,
          elapsedMs,
          stableDetected ? "yes" : "no",
          enoughObserveTime ? "yes" : "no",
          co2StableNow ? "yes" : "no",
          o2StableNow ? "yes" : "no",
          co2ppmRaw,
          co,
          h2s,
          o2,
          ch4,
          t_air,
          h_air);

        if (enoughObserveTime) {
          if (co2ppmRaw > 0) {
            fallback.co2ppm.add((float)co2ppmRaw);
          }
          fallback.co.add(co);
          fallback.h2s.add(h2s);
          fallback.o2.add(o2);
          fallback.ch4.add(ch4);
          fallback.airTemp.add(t_air);
          fallback.airHumidity.add(h_air);
        }

        // 一旦双通道判稳，后续样本才进入正式稳态统计集合。
        if (stableDetected) {
          if (co2ppmRaw > 0) {
            stable.co2ppm.add((float)co2ppmRaw);
          }
          stable.co.add(co);
          stable.h2s.add(h2s);
          stable.o2.add(o2);
          stable.ch4.add(ch4);
          stable.airTemp.add(t_air);
          stable.airHumidity.add(h_air);
        }

        if (elapsedMs >= staticMeasureWindowMs) {
          break;
        }

        unsigned long remainingMs = staticMeasureWindowMs - elapsedMs;
        delay(min(sampleIntervalMs, remainingMs));
      }

      // 优先使用正式稳态结果；如果本轮一直没判稳，则回退到 fallback，
      // 但只给参与判稳的通道标记 UNSTABLE，其它通道仍按自身值判断质量。
      const bool finalCo2Stable = lastEnoughObserveTime && latestCo2Stable;
      const bool finalO2Stable = lastEnoughObserveTime && latestO2Stable;
      const bool finalDualStable = finalCo2Stable && finalO2Stable && stable.co2ppm.count() > 0;
      const StableAverages& resultSet = finalDualStable ? stable : fallback;
      const char* co2Quality = finalCo2Stable ? nullptr : "UNSTABLE";
      const char* o2Quality = finalO2Stable ? nullptr : "UNSTABLE";
      if (!finalDualStable) {
        Serial.printf("[Measure] Point %u final dual-channel stability not satisfied, fallback to post-observation robust values (co2Stable=%s, o2Stable=%s)\n",
          (unsigned)(pointIndex + 1),
          finalCo2Stable ? "yes" : "no",
          finalO2Stable ? "yes" : "no");
      }
      Serial.println("[Measure] Point pump OFF, calculating robust stable values");

      float co2ppm = resultSet.co2ppm.robustAverageOr();
      float co2pct = (co2ppm > 0) ? (co2ppm / 10000.0f) : -1;
      float co = resultSet.co.robustAverageOr();
      float h2s = resultSet.h2s.robustAverageOr();
      float o2 = resultSet.o2.robustAverageOr();
      float ch4 = resultSet.ch4.robustAverageOr();
      float t_air = resultSet.airTemp.robustAverageOr();
      float h_air = resultSet.airHumidity.robustAverageOr();

      String ts = getTimeString();
      Serial.printf("[Measure] Point %u robust-stable timestamp=%s, usedCounts(CO2=%u CO=%u H2S=%u O2=%u CH4=%u Temp=%u RH=%u), finalDualStable=%s, co2Stable=%s, o2Stable=%s, CO2=%.2f %%VOL, CO=%.1f, H2S=%.1f, O2=%.2f, CH4=%.1f, Temp=%.1f, RH=%.1f\n",
        (unsigned)(pointIndex + 1),
        ts.c_str(),
        (unsigned)resultSet.co2ppm.count(),
        (unsigned)resultSet.co.count(),
        (unsigned)resultSet.h2s.count(),
        (unsigned)resultSet.o2.count(),
        (unsigned)resultSet.ch4.count(),
        (unsigned)resultSet.airTemp.count(),
        (unsigned)resultSet.airHumidity.count(),
        finalDualStable ? "yes" : "no",
        finalCo2Stable ? "yes" : "no",
        finalO2Stable ? "yes" : "no",
        co2pct,
        co,
        h2s,
        o2,
        ch4,
        t_air,
        h_air);

      String payload = "{";
      payload += "\"schema_version\":2,";
      payload += "\"ts\":\"" + ts + "\",";
      payload += "\"point_id\":" + String((unsigned)(pointIndex + 1)) + ",";
      payload += "\"controller_device_code\":\"" + appConfig.deviceCode + "\",";
      payload += "\"channels\":[";
      bool firstChannel = true;
      appendChannel(payload, firstChannel, "CO2", co2pct, 2, "%VOL", co2Quality);
      appendChannel(payload, firstChannel, "CO", co, 1, "ppm");
      appendChannel(payload, firstChannel, "H2S", h2s, 1, "ppm");
      appendChannel(payload, firstChannel, "O2", o2, 2, "%VOL", o2Quality);
      appendChannel(payload, firstChannel, "CH4", ch4, 1, "%LEL");
      appendChannel(payload, firstChannel, "AirTemp", t_air, 1, "℃");
      appendChannel(payload, firstChannel, "AirHumidity", h_air, 1, "%RH");
      payload += "]}";

      String postTopic = appConfig.mqttPostTopic(pointCode);
      Serial.printf("[Measure] Point %u payload size=%u bytes, topic=%s\n",
        (unsigned)(pointIndex + 1),
        (unsigned)payload.length(),
        postTopic.c_str());

      if (!publishDataOrCache(postTopic, payload, ts, 10000)) {
        cycleOk = false;
        Serial.printf("[Measure] Point %u publish failed, data was cached locally\n", (unsigned)(pointIndex + 1));
      }
      else {
        Serial.printf("[Measure] Point %u publish completed successfully\n", (unsigned)(pointIndex + 1));
      }
      savePointCompletion(cycleStartEpoch, pointIndex);
    }
    else {
      Serial.printf("[Measure] Point %u measurement already completed before restart, resuming from purge phase\n",
        (unsigned)(pointIndex + 1));
    }

    saveResumeState(true, pointIndex, ResumePhase::PurgePump);
    allPumpsOff();
    Serial.printf("[Measure] Purge pump ON for %lu ms\n", appConfig.purgePumpTime);
    pumpOn(PURGE_PUMP_INDEX);
    delay(appConfig.purgePumpTime);
    pumpOff(PURGE_PUMP_INDEX);
    Serial.println("[Measure] Purge pump OFF");
  }

  int uploaded = uploadCachedData(10);
  if (uploaded > 0) {
    Serial.printf("[Measure] Uploaded %d cached data items after cycle\n", uploaded);
  }

  allPumpsOff();
  clearPointCompletion();
  clearResumeState();
  g_measurementInProgress = false;
  Serial.printf("[Measure] Round-robin cycle finished with status=%s\n", cycleOk ? "OK" : "WARN");
  return cycleOk;
}

// =====================================================
// 任务：定时测量
// =====================================================
static void measurementTask(void*) {
  // 启动后延迟，确保与上线消息之间至少间隔 1000ms
  delay(1000);
  Serial.printf("[Measure] Scheduler started (interval=%lu ms)\n", appConfig.readInterval);

  if (g_initialMeasureDelayMs > 0) {
    Serial.printf("[Measure] Waiting %lu ms before first cycle\n", g_initialMeasureDelayMs);
    vTaskDelay(pdMS_TO_TICKS(g_initialMeasureDelayMs));
  }

  while (true) {
    unsigned long cycleStartMs = millis();
    bool cycleOk = true;
    if (g_resumePending) {
      Serial.printf("[Measure] Resuming interrupted cycle from point=%u, phase=%u\n",
        (unsigned)(g_resumePointIndex + 1),
        (unsigned)g_resumePhase);
      cycleOk = doMeasurementAndSave(g_resumePointIndex, g_resumePhase);
      g_resumePending = false;
      g_resumePointIndex = 0;
      g_resumePhase = ResumePhase::Idle;
    }
    else {
      Serial.println("[Measure] Starting scheduled cycle");
      cycleOk = doMeasurementAndSave();
    }

    if (!cycleOk) {
      Serial.println("[Measure] Cycle finished with warnings or deferred uploads; scheduler will continue and rely on cache retry");
    }

    unsigned long cycleDurationMs = millis() - cycleStartMs;
    if (cycleDurationMs < appConfig.readInterval) {
      unsigned long remainingMs = appConfig.readInterval - cycleDurationMs;
      Serial.printf("[Measure] Cycle duration=%lu ms, waiting remaining=%lu ms\n",
        cycleDurationMs, remainingMs);
      vTaskDelay(pdMS_TO_TICKS(remainingMs));
    }
    else {
      Serial.printf("[Measure] Cycle duration=%lu ms exceeded interval=%lu ms, starting next cycle immediately\n",
        cycleDurationMs, appConfig.readInterval);
    }
  }
}

// =====================================================
// 任务：执行队列命令
// =====================================================
static void commandTask(void*) {
  while (true) {
    time_t now = time(nullptr);
    std::vector<PendingCommand> dueCommands;
    if (g_cmdMutex) {
      xSemaphoreTake(g_cmdMutex, portMAX_DELAY);
    }
    for (int i = 0; i < (int)pendingCommands.size();) {
      if (now >= pendingCommands[i].targetTime) {
        dueCommands.push_back(pendingCommands[i]);
        pendingCommands.erase(pendingCommands.begin() + i);
        continue;
      }
      i++;
    }
    if (g_cmdMutex) {
      xSemaphoreGive(g_cmdMutex);
    }
    for (auto& cmd : dueCommands) {
      executeCommand(cmd);
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// =====================================================
// setup
// =====================================================
void setup() {
  Serial.begin(115200);
  Serial.println("[System] Boot started");
  if (!g_cmdMutex) {
    g_cmdMutex = xSemaphoreCreateMutex();
    if (!g_cmdMutex) {
      Serial.println("[CMD] Failed to create command mutex");
    }
  }

  // 1) SPIFFS + 读取 config.json
  if (!initSPIFFS() || !loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[System] Failed to load configuration, restarting");
    ESP.restart();
  }
  Serial.printf("[System] Config loaded: controller=%s, readInterval=%lu ms, intakeTime=%lu ms, staticWindow=%lu ms, autoStabilization=%lu ms, sampleInterval=%lu ms, purgePumpTime=%lu ms\n",
    appConfig.deviceCode.c_str(),
    appConfig.readInterval,
    effectiveSampleIntakeMs(),
    effectiveStaticMeasureWindowMs(),
    effectiveSampleStabilizationMs(),
    effectiveSampleIntervalMs(),
    appConfig.purgePumpTime);
  logCycleBudget("[System]");

  // 2) 初始化数据缓存模块
  if (!initDataBuffer(200, 7)) {
    Serial.println("[System] Data buffer initialization failed, continuing without full cache support");
  }
  else {
    Serial.println("[System] Data buffer initialized successfully");
  }

  // 3) WiFi + NTP
  if (!connectToWiFi(20000) || !multiNTPSetup(20000)) {
    Serial.println("[System] WiFi or NTP setup failed, restarting");
    ESP.restart();
  }

  // 4) MQTT
  if (!connectToMQTT(20000)) {
    Serial.println("[System] MQTT connection failed, restarting");
    ESP.restart();
  }

  getMQTTClient().setCallback(mqttCallback);

  // 订阅 response topic（根据 device_code 自动生成）
  String respTopic = appConfig.mqttResponseTopic();
  if (respTopic.length() > 0) {
    getMQTTClient().subscribe(respTopic.c_str());
    Serial.println("[MQTT] Subscribed: " + respTopic);
  }
  else {
    Serial.println("[MQTT] Response topic is empty, subscription skipped");
  }

  // 5) 传感器初始化
  // 这里默认使用:
  // MH-Z16 -> Serial1 RX=18 TX=19
  // ZCE04B -> Serial2 RX=16 TX=17
  // SHT30  -> I2C SDA=21 SCL=22
  // Pumps  -> P1=13 P2=14 P3=25 P4=26 P5=27 P6=32 Purge=33
  if (!initSensorAndPump(
    PUMP_PINS,
    TOTAL_PUMP_COUNT,
    Serial1,
    PIN_MHZ16_RX,
    PIN_MHZ16_TX,
    Serial2,
    PIN_ZCE04B_RX,
    PIN_ZCE04B_TX,
    PIN_SHT30_SDA,
    PIN_SHT30_SCL,
    5000)) {
    Serial.println("[ERR] Sensor initialization failed, restarting");
    ESP.restart();
  }
  Serial.println("[System] Sensors initialized successfully");

  // 预热 MH-Z16
  Serial.println("[System] Preheating MH-Z16 sensor");
  readMHZ16();
  delay(500);

  // 6) 发布上线消息（先告知服务器设备已上线）
  Serial.println("[System] Publishing registration message...");
  publishOnlineWithConfig();
  delay(500);  // 等待上线消息完全发送

  // 7) 上传之前缓存的旧数据（如果有）
  int pendingCount = uploadCachedData(10);
  if (pendingCount > 0) {
    Serial.printf("[System] Uploaded %d cached history item(s) during startup\n", pendingCount);
  }
  else {
    Serial.println("[System] No cached history uploaded during startup");
  }

  // 8) 恢复上次测量时间（基于上一次轮询开始时间恢复节奏）
  bool resumeHandled = false;
  if (preferences.begin(NVS_NAMESPACE, false)) {
    unsigned long lastSec = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    bool resumeActive = preferences.getBool(NVS_KEY_RESUME_ACTIVE, false);
    unsigned int resumePoint = preferences.getUInt(NVS_KEY_RESUME_POINT, 0);
    uint8_t resumePhase = preferences.getUChar(NVS_KEY_RESUME_PHASE, (uint8_t)ResumePhase::Idle);
    unsigned long doneCycle = preferences.getULong(NVS_KEY_DONE_CYCLE, 0);
    unsigned int donePoint = preferences.getUInt(NVS_KEY_DONE_POINT, 0);
    time_t nowSec = time(nullptr);
    Serial.printf("[Time] Last cycle start epoch from NVS=%lu, current epoch=%lu\n",
      lastSec, (unsigned long)nowSec);
    Serial.printf("[State] Resume state from NVS: active=%s, point=%u, phase=%u\n",
      resumeActive ? "true" : "false",
      resumePoint + 1,
      resumePhase);
    Serial.printf("[State] Point completion marker: cycle=%lu, point=%u\n",
      doneCycle,
      donePoint + 1);

    if (resumeActive && resumePoint < POINT_COUNT) {
      g_resumePending = true;
      g_resumePointIndex = resumePoint;
      g_resumePhase = (ResumePhase)resumePhase;
      if (g_resumePhase == ResumePhase::PointPump &&
        doneCycle == lastSec &&
        donePoint == resumePoint) {
        g_resumePhase = ResumePhase::PurgePump;
        Serial.printf("[State] Adjusted resume phase to purge for point=%u based on completion marker\n",
          (unsigned)(g_resumePointIndex + 1));
      }
      g_initialMeasureDelayMs = 0;
      resumeHandled = true;
      Serial.printf("[State] Will resume interrupted cycle from point=%u, phase=%u\n",
        (unsigned)(g_resumePointIndex + 1),
        (unsigned)g_resumePhase);
    }
    else if (lastSec > 0 && nowSec > (time_t)lastSec) {
      // 计算上次轮询开始到现在经过的时间
      unsigned long elapsedMs = (unsigned long)(nowSec - (time_t)lastSec) * 1000UL;
      Serial.printf("[Time] Elapsed since last cycle start=%lu ms\n", elapsedMs);

      if (elapsedMs >= appConfig.readInterval) {
        g_initialMeasureDelayMs = 0;
        Serial.println("[Time] Interval already elapsed, first cycle will start immediately");
      }
      else {
        g_initialMeasureDelayMs = appConfig.readInterval - elapsedMs;
        Serial.printf("[Time] Remaining wait before first cycle=%lu ms\n", g_initialMeasureDelayMs);
      }
    }
    else {
      g_initialMeasureDelayMs = 0;
      Serial.println("[Time] No valid previous cycle record, starting immediately");
    }
    preferences.end();
  }
  else {
    g_initialMeasureDelayMs = 0;
    Serial.println("[Time] Failed to open NVS, scheduler starts from now");
  }
  if (resumeHandled) {
    Serial.println("[State] Resume handling is active, first cycle will continue from saved state");
  }

  // 9) 启动任务
  xTaskCreatePinnedToCore(measurementTask, "Measure", 16384, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(commandTask, "Command", 8192, NULL, 1, NULL, 1);

  Serial.println("[System] Initialization complete");
}

// =====================================================
// loop
// =====================================================
void loop() {
  maintainMQTT(30000);  // 增加超时时间，给网络更多恢复时间
  if (g_pendingRestart && (int32_t)(millis() - g_restartAtMs) >= 0) {
    ESP.restart();
  }
  static unsigned long lastCacheUploadMs = 0;
  unsigned long now = millis();
  if (now - lastCacheUploadMs >= CACHE_UPLOAD_INTERVAL_MS) {
    lastCacheUploadMs = now;
    int uploaded = uploadCachedData(10);
    if (uploaded > 0) {
      Serial.printf("[Loop] Uploaded %d cached data items\n", uploaded);
    }
  }
  delay(100);
}
