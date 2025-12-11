#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"
#include "log_manager.h"
#include <ArduinoJson.h>
#include <vector>

// ========== 全局持久化变量与任务变量 ==========
Preferences preferences;
static const char* NVS_NAMESPACE = "my-nvs";
static const char* NVS_KEY_LAST_MEAS = "lastMeas";
static unsigned long prevMeasureMs = 0;

// ========== 延迟命令结构体 ==========
struct PendingCommand {
  String cmd;
  String action;
  unsigned long duration;
  time_t targetTime;
};
std::vector<PendingCommand> pendingCommands;

// ========== 命令执行 ==========
void executeCommand(const PendingCommand& pcmd) {
  Serial.printf("[CMD] 执行：%s %s 持续 %lu ms\n",
    pcmd.cmd.c_str(), pcmd.action.c_str(), pcmd.duration);

  if (pcmd.cmd == "aeration") {
    if (pcmd.action == "on") {
      aerationOn();
      if (pcmd.duration > 0) {
        delay(pcmd.duration);
        aerationOff();
      }
    }
    else aerationOff();
  }
  else if (pcmd.cmd == "exhaust") {
    if (pcmd.action == "on") {
      exhaustPumpOn();
      if (pcmd.duration > 0) {
        delay(pcmd.duration);
        exhaustPumpOff();
      }
    }
    else exhaustPumpOff();
  }
  else {
    Serial.println("[CMD] 未知命令类型：" + pcmd.cmd);
  }
}

// ========== MQTT 消息处理 ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.println("[MQTT] 收到控制指令");

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, payload, length)) {
    Serial.println("[MQTT] JSON 解析失败");
    return;
  }

  String device = doc["device"] | "";
  if (device != appConfig.equipmentKey) {
    Serial.println("[MQTT] 设备不匹配，忽略");
    return;
  }

  JsonArray cmds = doc["commands"].as<JsonArray>();
  if (cmds.isNull()) {
    Serial.println("[MQTT] 无 commands 数组");
    return;
  }

  for (JsonVariant v : cmds) {
    JsonObject obj = v.as<JsonObject>();

    String cmd = obj["command"] | "";
    String action = obj["action"] | "";
    unsigned long dur = obj["duration"] | 0;
    String schedule = obj["schedule"] | "";

    time_t target = time(nullptr);
    if (schedule.length() > 0) {
      struct tm tm_s = {};
      if (strptime(schedule.c_str(), "%Y-%m-%d %H:%M:%S", &tm_s))
        target = mktime(&tm_s);
      else {
        Serial.println("[MQTT] schedule 格式错误");
        continue;
      }
    }

    pendingCommands.push_back({ cmd, action, dur, target });
  }
}

// ========== 采样与上传（已加入 DHT22） ==========
bool doMeasurementAndSave() {
  Serial.println("[Measure] 开始采样");

  // 抽气采样
  exhaustPumpOn();
  delay(appConfig.pumpRunTime);
  exhaustPumpOff();

  // ==== CO₂ 多次测量取平均 ====
  const int sampleCount = 3;
  std::vector<int> co2Samples;
  for (int i = 0; i < sampleCount; i++) {
    int v = readMHZ16();
    if (v > 0) co2Samples.push_back(v);
    delay(200);
  }
  float co2ppm = -1;
  if (!co2Samples.empty()) {
    float sum = 0;
    for (int v : co2Samples) sum += v;
    co2ppm = sum / co2Samples.size();
  }
  float co2pct = (co2ppm > 0) ? co2ppm / 10000.0f : -1;

  // ==== 读取各类传感器 ====
  float o2 = readEOxygen();
  float t_ds18b20 = readDS18B20();
  float mois = readFDS100(34);

  // ★★★ 新增：DHT22 气体温湿度 ★★★
  float t_air = readDHT22Temp();
  float h_air = readDHT22Hum();

  String ts = getTimeString();
  time_t nowEpoch = time(nullptr);

  // ==== 构造 JSON Payload ====
  String payload = "{ \"data\":[";

  payload += "{\"value\":" + String(co2pct, 2) + ",\"key\":\"" + appConfig.keyCO2 + "\",\"measured_time\":\"" + ts + "\"},";
  payload += "{\"value\":" + String(o2, 2) + ",\"key\":\"" + appConfig.keyO2 + "\",\"measured_time\":\"" + ts + "\"},";
  payload += "{\"value\":" + String(t_ds18b20, 1) + ",\"key\":\"" + appConfig.keyRoomTemp + "\",\"measured_time\":\"" + ts + "\"},";
  payload += "{\"value\":" + String(mois, 1) + ",\"key\":\"" + appConfig.keyMois + "\",\"measured_time\":\"" + ts + "\"},";

  // ★★★ 新增上传字段 ★★★
  payload += "{\"value\":" + String(t_air, 1) + ",\"key\":\"" + appConfig.keyAirTemp + "\",\"measured_time\":\"" + ts + "\"},";
  payload += "{\"value\":" + String(h_air, 1) + ",\"key\":\"" + appConfig.keyAirHum + "\",\"measured_time\":\"" + ts + "\"}";

  payload += "]}";

  // ==== 上传 ====
  if (publishData(appConfig.mqttPostTopic, payload, 10000)) {
    preferences.begin(NVS_NAMESPACE, false);
    preferences.putULong(NVS_KEY_LAST_MEAS, nowEpoch);
    preferences.end();
    Serial.println("[Measure] 上传成功");
    return true;
  }

  Serial.println("[Measure] 上传失败");
  return false;
}

// ========== 周期任务 ==========
void measurementTask(void* pvParameters) {
  while (true) {
    if (millis() - prevMeasureMs >= appConfig.readInterval) {
      prevMeasureMs = millis();
      doMeasurementAndSave();
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

void commandTask(void* pvParameters) {
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

// ========== 主初始化 ==========
void setup() {
  Serial.begin(115200);
  Serial.println("[System] 启动中...");

  initLogSystem();
  setMinLogLevel(LogLevel::INFO);

  if (!initSPIFFS() || !loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[ERR] 配置加载失败");
    ESP.restart();
  }

  if (!connectToWiFi(20000) || !multiNTPSetup(20000)) ESP.restart();
  if (!connectToMQTT(20000)) ESP.restart();

  getMQTTClient().setCallback(mqttCallback);
  getMQTTClient().subscribe(appConfig.mqttResponseTopic.c_str());

  // ★★★ 传感器初始化：已包含 DHT22 ★★★
  if (!initSensorAndPump(25, 26, Serial1, 16, 17, /*DHT22 pin*/ 14, 5000)) {
    Serial.println("[ERR] 传感器初始化失败");
    ESP.restart();
  }

  readMHZ16();
  delay(500);

  // ========== 加载上次测量时间 ==========
  if (preferences.begin(NVS_NAMESPACE, false)) {
    unsigned long lastSec = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    time_t nowSec = time(nullptr);

    unsigned long interval = appConfig.readInterval / 1000UL;
    unsigned long elapsed = (nowSec > lastSec) ? nowSec - lastSec : 0;

    if (elapsed >= interval)
      prevMeasureMs = millis() - appConfig.readInterval;
    else
      prevMeasureMs = millis() - (appConfig.readInterval - (interval - elapsed) * 1000UL);

    preferences.end();
  }

  // ========== 发送上线状态 ==========
  String bootMsg = "{";
  bootMsg += "\"device\":\"" + appConfig.equipmentKey + "\",";
  bootMsg += "\"status\":\"online\",";
  bootMsg += "\"timestamp\":\"" + getTimeString() + "\"";
  bootMsg += "}";
  publishData(appConfig.mqttPostTopic, bootMsg, 10000);

  // 启动任务
  xTaskCreatePinnedToCore(measurementTask, "Measure", 8192, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(commandTask, "Command", 8192, NULL, 1, NULL, 1);

  Serial.println("[System] 初始化完成");
}

// ========== 主循环 ==========
void loop() {
  maintainMQTT(5000);
  delay(100);
}
