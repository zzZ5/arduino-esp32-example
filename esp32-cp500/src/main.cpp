#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"
#include "log_manager.h"
#include <ArduinoJson.h>
#include <vector>

// ========================= 持久化存储设置 =========================
Preferences preferences;
static const char* NVS_NAMESPACE = "my-nvs";              // NVS 命名空间
static const char* NVS_KEY_LAST_MEAS = "lastMeas";        // 上次测量时间的 key
static const char* NVS_KEY_LAST_AERATION = "lastAer";     // 上次曝气时间的 key
static unsigned long prevMeasureMs = 0;                   // 上次测量的毫秒时间戳
static unsigned long preAerationMs = 0;                   // 上次曝气的毫秒时间戳

// ========================= 命令队列结构体 =========================
// 用于存储定时控制命令（如曝气、加热器）
struct PendingCommand {
  String cmd;              // 控制命令名（"aeration"、"heater"）
  String action;           // 操作指令（"on"、"off"）
  unsigned long duration;  // 控制持续时间（毫秒）
  time_t targetTime;       // 预定执行时间（Unix 时间戳）
};
std::vector<PendingCommand> pendingCommands;


// ========================= 泵水保护控制变量 =========================
unsigned long pumpStartMs = 0;                 // 上次泵开启的时间


// ========================= 全局控制状态变量 =========================
bool heaterIsOn = false;     // 加热器状态（true=开启）
bool pumpIsOn = false;   // 曝气状态（true=开启）
bool aerationIsOn = false;   // 曝气状态（true=开启）

// ========================= 控制命令执行函数 =========================
// 执行控制命令（如开关加热器或风机）
void executeCommand(const PendingCommand& pcmd) {
  Serial.printf("[CMD] 执行：%s %s 持续 %lu ms\n", pcmd.cmd.c_str(), pcmd.action.c_str(), pcmd.duration);
  if (pcmd.cmd == "aeration") {
    if (pcmd.action == "on") {
      aerationOn();
      aerationIsOn = true;
      if (pcmd.duration > 0) {
        delay(pcmd.duration);
        aerationOff();
        aerationIsOn = false;
      }
    }
    else {
      aerationOff();
      aerationIsOn = false;
    }
  }
  else if (pcmd.cmd == "heater") {
    if (pcmd.action == "on") {
      heaterOn();
      heaterIsOn = true;
      if (pcmd.duration > 0) {
        delay(pcmd.duration);
        heaterOff();
        heaterIsOn = false;
      }
    }
    else {
      heaterOff();
      heaterIsOn = false;
    }
  }
  else if (pcmd.cmd == "pump") {
    if (pcmd.action == "on") {
      pumpOn();
      pumpIsOn = true;
      if (pcmd.duration > 0) {
        delay(pcmd.duration);
        pumpOff();
        pumpIsOn = false;
      }
    }
    else {
      pumpOff();
      pumpIsOn = false;
    }
  }
  else {
    Serial.println("[CMD] 未知命令：" + pcmd.cmd);
  }
}

// ========================= config更新函数 =========================
// 解析远程发送的 JSON 配置文件并进行更新，成功返回 true
bool updateAppConfigFromJson(JsonObject obj) {
  // 1. WiFi 参数
  if (obj.containsKey("wifi")) {
    JsonObject wifi = obj["wifi"];
    if (wifi.containsKey("ssid"))     appConfig.wifiSSID = wifi["ssid"].as<String>();
    if (wifi.containsKey("password")) appConfig.wifiPass = wifi["password"].as<String>();
  }

  // 2. MQTT 参数
  if (obj.containsKey("mqtt")) {
    JsonObject mqtt = obj["mqtt"];
    if (mqtt.containsKey("server"))         appConfig.mqttServer = mqtt["server"].as<String>();
    if (mqtt.containsKey("port"))           appConfig.mqttPort = mqtt["port"].as<uint16_t>();
    if (mqtt.containsKey("user"))           appConfig.mqttUser = mqtt["user"].as<String>();
    if (mqtt.containsKey("pass"))           appConfig.mqttPass = mqtt["pass"].as<String>();
    if (mqtt.containsKey("clientId"))       appConfig.mqttClientId = mqtt["clientId"].as<String>();
    if (mqtt.containsKey("post_topic"))     appConfig.mqttPostTopic = mqtt["post_topic"].as<String>();
    if (mqtt.containsKey("response_topic")) appConfig.mqttResponseTopic = mqtt["response_topic"].as<String>();
  }

  // 3. NTP 服务器
  if (obj.containsKey("ntp_host") && obj["ntp_host"].is<JsonArray>()) {
    JsonArray ntpArr = obj["ntp_host"].as<JsonArray>();
    appConfig.ntpServers.clear();
    for (JsonVariant v : ntpArr) {
      appConfig.ntpServers.push_back(v.as<String>());
    }
  }

  // 4. 控制参数
  if (obj.containsKey("post_interval")) appConfig.postInterval = obj["post_interval"].as<uint32_t>();
  if (obj.containsKey("temp_maxdif"))   appConfig.tempMaxDiff = obj["temp_maxdif"].as<uint32_t>();

  // 5. 温度限制参数
  if (obj.containsKey("temp_limitout_max")) appConfig.tempLimitOutMax = obj["temp_limitout_max"].as<uint32_t>();
  if (obj.containsKey("temp_limitout_min")) appConfig.tempLimitOutMin = obj["temp_limitout_min"].as<uint32_t>();
  if (obj.containsKey("temp_limitin_max"))  appConfig.tempLimitInMax = obj["temp_limitin_max"].as<uint32_t>();
  if (obj.containsKey("temp_limitin_min"))  appConfig.tempLimitInMin = obj["temp_limitin_min"].as<uint32_t>();

  // 6. 设备编号
  if (obj.containsKey("equipment_key")) appConfig.equipmentKey = obj["equipment_key"].as<String>();

  // 7. 传感器 key
  if (obj.containsKey("keys")) {
    JsonObject keys = obj["keys"];
    if (keys.containsKey("temp_in")) {
      appConfig.keyTempIn = keys["temp_in"].as<String>();
    }
    if (keys.containsKey("temp_out") && keys["temp_out"].is<JsonArray>()) {
      appConfig.keyTempOut.clear();
      for (JsonVariant v : keys["temp_out"].as<JsonArray>()) {
        appConfig.keyTempOut.push_back(v.as<String>());
      }
    }
  }

  // 8. 曝气定时器参数
  if (obj.containsKey("aeration_timer")) {
    JsonObject aeration = obj["aeration_timer"];
    if (aeration.containsKey("enabled"))  appConfig.aerationTimerEnabled = aeration["enabled"].as<bool>();
    if (aeration.containsKey("interval")) appConfig.aerationInterval = aeration["interval"].as<uint32_t>();
    if (aeration.containsKey("duration")) appConfig.aerationDuration = aeration["duration"].as<uint32_t>();
  }

  // 9. 水泵运行参数
  if (obj.containsKey("pump_max_duration")) appConfig.pumpMaxDuration = obj["pump_max_duration"].as<uint32_t>();

  return true;
}

// ========================= MQTT 消息回调处理 =========================
// 解析远程发送的 JSON 控制命令并加入待执行队列
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println("[MQTT] JSON 解析错误：" + String(err.c_str()));
    return;
  }

  String device = doc["device"] | "";
  if (device != appConfig.equipmentKey) return;

  JsonArray cmds = doc["commands"].as<JsonArray>();
  if (cmds.isNull()) return;

  for (JsonVariant v : cmds) {
    JsonObject obj = v.as<JsonObject>();
    String cmd = obj["command"] | "";
    String action = obj["action"] | "";
    unsigned long duration = obj["duration"] | 0;
    String schedule = obj["schedule"] | "";

    // 检查命令是否是配置更新
    if (cmd == "config_update") {
      JsonObject cfg = obj["config"].as<JsonObject>();
      if (!cfg.isNull()) {
        if (updateAppConfigFromJson(cfg)) {
          // 保存更新后的配置
          if (saveConfigToSPIFFS("/config.json")) {
            Serial.println("[CMD] ✅ 配置已远程更新并保存");
            // 配置保存成功后，重启设备
            ESP.restart();
          }
          else {
            Serial.println("[CMD] ❌ 配置保存失败");
          }
        }
        else {
          Serial.println("[CMD] ❌ 配置更新失败");
        }
      }
      break;  // 跳过其他命令的执行
    }


    time_t target = time(nullptr);
    if (schedule.length() > 0) {
      struct tm schedTime = {};
      if (strptime(schedule.c_str(), "%Y-%m-%d %H:%M:%S", &schedTime)) {
        target = mktime(&schedTime);
      }
      else {
        Serial.println("[MQTT] 错误的时间格式");
        continue;
      }
    }

    pendingCommands.push_back({ cmd, action, duration, target });
  }
}

// ========================= 中位数计算工具函数 =========================
float median(std::vector<float> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  size_t mid = values.size() / 2;
  return (values.size() % 2 == 0) ? (values[mid - 1] + values[mid]) / 2.0 : values[mid];
}

// ========================= 曝气控制函数 =========================
void checkAndControlAerationByTimer() {
  if (!appConfig.aerationTimerEnabled) return;
  unsigned long nowMs = millis();

  // 状态1：未在曝气中，检查是否应启动曝气
  if (!aerationIsOn && (nowMs - preAerationMs >= appConfig.aerationInterval)) {
    Serial.printf("[Aeration] 到达曝气时间，开始曝气 %lu ms\n", appConfig.aerationDuration);
    aerationOn();
    aerationIsOn = true;
    preAerationMs = nowMs;  // 同时作为当前轮次的开始时间
    // 保存持久化状态
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_AERATION, preAerationMs);
      preferences.end();
    }
  }

  // 状态2：正在曝气中，检查是否应停止曝气
  if (aerationIsOn && (nowMs - preAerationMs >= appConfig.aerationDuration)) {
    Serial.println("[Aeration] 曝气时间到，停止曝气");
    aerationOff();
    aerationIsOn = false;
  }
}


// ========================= 主测量函数 =========================
// 完成一次采集、状态控制、数据上传、记录时间
bool doMeasurementAndSave() {
  Serial.println("[Measure] 采集温度");

  float t_in = readTempIn();                     // 内部温度
  std::vector<float> t_outs = readTempOut();     // 外部温度（多个）
  String ts = getTimeString();                   // 时间字符串
  time_t nowEpoch = time(nullptr);               // 当前 Unix 时间戳

  // 计算温差最大值判断是否合理
  float max_diff = 0.0;
  for (float t : t_outs) {
    float diff = abs(t - t_in);
    if (diff > max_diff) max_diff = diff;
  }
  bool diffOk = (max_diff <= appConfig.tempMaxDiff);

  // 获取外部温度中位数
  float med_out = median(t_outs);

  // 温度保护与加热判断
  float out_max = appConfig.tempLimitOutMax;
  float out_min = appConfig.tempLimitOutMin;
  float in_max = appConfig.tempLimitInMax;
  float in_min = appConfig.tempLimitInMin;

  bool needHeat = false;
  bool overheat = false;

  if (med_out >= out_max) {
    Serial.printf("[Heat] 外部温度 %.2f ≥ 上限 %.2f，关闭加热器\n", med_out, out_max);
    overheat = true;
  }
  else if (med_out <= out_min) {
    Serial.printf("[Heat] 外部温度 %.2f ≤ 下限 %.2f，开启加热器\n", med_out, out_min);
    needHeat = true;
  }

  if (t_in >= in_max) {
    Serial.printf("[Heat] 内部温度 %.2f ≥ 上限 %.2f，关闭加热器\n", t_in, in_max);
    overheat = true;
  }
  else if (t_in <= in_min) {
    Serial.printf("[Heat] 内部温度 %.2f ≤ 下限 %.2f，开启加热器\n", t_in, in_min);
    needHeat = true;
  }

  // 若未过热，则根据温差判断是否开启加热
  if (!needHeat && !overheat) {
    float diff = t_in - med_out;
    float max_allowed_diff = pow((t_in - appConfig.tempMaxDiff) / 40.0f, 2);
    if (diff >= max_allowed_diff) {
      Serial.printf("[Heat] 温差 %.2f 超过阈值 %.2f，开启加热器\n", diff, max_allowed_diff);
      needHeat = true;
    }
    else {
      Serial.printf("[Heat] 温差 %.2f 未超过阈值 %.2f，关闭加热器\n", diff, max_allowed_diff);
    }
  }

  // 执行加热控制
  if (overheat) {
    heaterOff(); heaterIsOn = false;
    pumpOff(); pumpIsOn = false;
  }
  else if (needHeat) {
    heaterOn(); heaterIsOn = true;
    pumpOn(); pumpIsOn = true;
  }
  else {
    heaterOff(); heaterIsOn = false;
    if (!pumpIsOn) {
      pumpOn(); pumpIsOn = true;
      pumpStartMs = millis();
    }
  }

  // ===== 曝气定时判断 =====
  checkAndControlAerationByTimer();


  // ===== 泵水保护逻辑（仅在 heater 关闭时才生效）=====

  if (!heaterIsOn && pumpIsOn && (millis() - pumpStartMs >= appConfig.pumpMaxDuration)) {
    Serial.println("[Pump] 泵运行超时（非加热状态），自动关闭");
    pumpOff(); pumpIsOn = false;
  }

  // 构建 JSON 数据并上传
  StaticJsonDocument<1024> doc;
  JsonArray data = doc.createNestedArray("data");

  JsonObject obj_in = data.createNestedObject();
  obj_in["key"] = appConfig.keyTempIn;
  obj_in["value"] = t_in;
  obj_in["measured_time"] = ts;

  for (size_t i = 0; i < t_outs.size(); ++i) {
    JsonObject obj = data.createNestedObject();
    if (i < appConfig.keyTempOut.size()) {
      obj["key"] = appConfig.keyTempOut[i];
    }
    else {
      obj["key"] = appConfig.keyTempOut[0] + "_X" + String(i);
    }
    obj["value"] = t_outs[i];
    obj["measured_time"] = ts;
  }

  // 系统状态字段
  doc["info"]["diff_ok"] = diffOk;
  doc["info"]["heat"] = heaterIsOn;
  doc["info"]["pump"] = pumpIsOn;
  doc["info"]["aeration"] = aerationIsOn;

  String payload;
  serializeJson(doc, payload);

  if (publishData(appConfig.mqttPostTopic, payload, 10000)) {
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_MEAS, nowEpoch);
      preferences.end();
    }
    return true;
  }
  return false;
}

// ========================= 测量任务 =========================
void measurementTask(void* pv) {
  while (true) {
    if (millis() - prevMeasureMs >= appConfig.postInterval) {
      prevMeasureMs = millis();
      doMeasurementAndSave();
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ========================= 命令调度任务 =========================
void commandTask(void* pv) {
  while (true) {
    time_t now = time(nullptr);
    for (int i = 0; i < pendingCommands.size(); i++) {
      if (now >= pendingCommands[i].targetTime) {
        executeCommand(pendingCommands[i]);
        pendingCommands.erase(pendingCommands.begin() + i);
        i--;
      }
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}


// ========================= 初始化函数 =========================
void setup() {
  Serial.begin(115200);
  Serial.println("[System] 启动中");

  initLogSystem();
  if (!initSPIFFS() || !loadConfigFromSPIFFS("/config.json")) {
    ESP.restart();
  }
  printConfig(appConfig);  // 输出配置信息

  if (!connectToWiFi(20000) || !multiNTPSetup(20000)) {
    ESP.restart();
  }

  if (!connectToMQTT(20000)) {
    ESP.restart();
  }

  getMQTTClient().setCallback(mqttCallback);
  getMQTTClient().subscribe(appConfig.mqttResponseTopic.c_str());

  if (!initSensors(4, 5, 25, 26, 27)) {
    ESP.restart();
  }

  // 恢复测量定时
  if (preferences.begin(NVS_NAMESPACE, false)) {
    unsigned long lastSec = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    preAerationMs = preferences.getULong(NVS_KEY_LAST_AERATION, 0);
    time_t nowSec = time(nullptr);
    unsigned long intervalSec = appConfig.postInterval / 1000UL;
    unsigned long elapsedSec = (nowSec > lastSec) ? nowSec - lastSec : 0;
    if (elapsedSec >= intervalSec) {
      prevMeasureMs = millis() - appConfig.postInterval;
    }
    else {
      prevMeasureMs = millis() - (appConfig.postInterval - elapsedSec * 1000UL);
    }
    preferences.end();
  }
  else {
    prevMeasureMs = millis() - appConfig.postInterval;
  }


  // ========== 上线状态上传 ==========
  String nowStr = getTimeString();
  String lastMeasStr = "unknown";
  if (preferences.begin(NVS_NAMESPACE, false)) {
    unsigned long lastMeasSec = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    if (lastMeasSec > 0) {
      struct tm* tm_info = localtime((time_t*)&lastMeasSec);
      char buffer[32];
      strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm_info);
      lastMeasStr = String(buffer);
    }
    preferences.end();
  }
  String bootMsg = "{";
  bootMsg += "\"device\":\"" + appConfig.equipmentKey + "\",";
  bootMsg += "\"status\":\"online\",";
  bootMsg += "\"timestamp\":\"" + nowStr + "\",";
  bootMsg += "\"last_measure_time\":\"" + lastMeasStr + "\"";
  bootMsg += "}";

  bool ok = publishData(appConfig.mqttPostTopic, bootMsg, 10000);
  Serial.println(ok ? "[MQTT] 上线消息发送成功" : "[MQTT] 上线消息发送失败");
  Serial.println("[MQTT] Payload: " + bootMsg);

  // ========== 启动任务 ==========
  xTaskCreatePinnedToCore(measurementTask, "MeasureTask", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(commandTask, "CommandTask", 4096, NULL, 1, NULL, 1);

  Serial.println("[System] 启动完成");
}

// ========================= 主循环 =========================
void loop() {
  maintainMQTT(5000);
  delay(100);
}