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
static unsigned long prevMeasureMs = 0;  // 用于定时测量任务（millis）

// ========== 延迟命令结构体 ==========
struct PendingCommand {
  String cmd;
  String action;
  unsigned long duration;
  time_t targetTime;
};
std::vector<PendingCommand> pendingCommands;

// ========== 命令执行函数 ==========
void executeCommand(const PendingCommand& pcmd) {
  Serial.printf("[CMD] 执行：%s %s 持续 %lu ms\n", pcmd.cmd.c_str(), pcmd.action.c_str(), pcmd.duration);
  if (pcmd.cmd == "aeration") {
    if (pcmd.action == "on") {
      aerationOn();
      if (pcmd.duration > 0) {
        delay(pcmd.duration);
        aerationOff();
      }
    }
    else {
      aerationOff();
    }
  }
  else if (pcmd.cmd == "exhaust") {
    if (pcmd.action == "on") {
      exhaustPumpOn();
      if (pcmd.duration > 0) {
        delay(pcmd.duration);
        exhaustPumpOff();
      }
    }
    else {
      exhaustPumpOff();
    }
  }
  else {
    Serial.println("[CMD] 未知命令类型：" + pcmd.cmd);
  }
}

// ========== MQTT 消息回调处理 ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.println("[MQTT] 收到控制指令");
  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.println("[MQTT] JSON 解析错误：" + String(err.c_str()));
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
    unsigned long duration = obj["duration"] | 0;
    String schedule = obj["schedule"] | "";

    time_t target = time(nullptr);
    if (schedule.length() > 0) {
      struct tm schedTime = {};
      if (strptime(schedule.c_str(), "%Y-%m-%d %H:%M:%S", &schedTime)) {
        target = mktime(&schedTime);
      }
      else {
        Serial.println("[MQTT] 错误的 schedule 格式");
        continue;
      }
    }

    pendingCommands.push_back({ cmd, action, duration, target });
    Serial.printf("[MQTT] 已加入命令队列：%s %s\n", cmd.c_str(), action.c_str());
  }
}

// ========== 执行一次测量并上传数据 ==========
bool doMeasurementAndSave() {
  Serial.println("[Measure] 开始采样测量");
  exhaustPumpOn();
  delay(appConfig.pumpRunTime);
  exhaustPumpOff();

  // 进行多次 CO2 测量并去除异常值后取平均值
  const int sampleCount = 3;
  std::vector<int> co2Samples;
  for (int i = 0; i < sampleCount; ++i) {
    int val = readMHZ16();
    if (val > 0) {
      co2Samples.push_back(val);
    }
    delay(200);
  }
  float co2ppm = -1;
  if (!co2Samples.empty()) {
    float sum = 0;
    for (int v : co2Samples) sum += v;
    co2ppm = sum / co2Samples.size();
  }
  float co2pct = (co2ppm > 0) ? (co2ppm / 10000.0f) : -1.0f;

  float o2 = readEOxygen();
  float temp = readDS18B20();
  float mois = readFDS100(34);
  String ts = getTimeString();
  time_t nowEpoch = time(nullptr);

  String payload = "{ \"data\":[";
  payload += "{\"value\":" + String(co2pct, 2) + ",\"key\":\"" + appConfig.keyCO2 + "\",\"measured_time\":\"" + ts + "\"},";
  payload += "{\"value\":" + String(o2, 2) + ",\"key\":\"" + appConfig.keyO2 + "\",\"measured_time\":\"" + ts + "\"},";
  payload += "{\"value\":" + String(temp, 1) + ",\"key\":\"" + appConfig.keyRoomTemp + "\",\"measured_time\":\"" + ts + "\"},";
  payload += "{\"value\":" + String(mois, 1) + ",\"key\":\"" + appConfig.keyMois + "\",\"measured_time\":\"" + ts + "\"}";
  payload += "]}";

  if (publishData(appConfig.mqttPostTopic, payload, 10000)) {
    if (preferences.begin(NVS_NAMESPACE, false)) {
      preferences.putULong(NVS_KEY_LAST_MEAS, nowEpoch);
      preferences.end();
    }
    Serial.println("[Measure] 数据上传成功");
    return true;
  }
  else {
    Serial.println("[Measure] 数据上传失败");
    return false;
  }
}

// ========== 周期测量任务 ==========
void measurementTask(void* pvParameters) {
  Serial.println("[Task] 周期测量任务已启动");
  while (true) {
    if (millis() - prevMeasureMs >= appConfig.readInterval) {
      prevMeasureMs = millis();
      doMeasurementAndSave();
    }
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ========== 延迟命令任务 ==========
void commandTask(void* pvParameters) {
  Serial.println("[Task] 指令任务已启动");
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

// ========== 主初始化函数 ==========
void setup() {
  Serial.begin(115200);
  Serial.println("[System] 设备启动中...");

  initLogSystem();
  setMinLogLevel(LogLevel::INFO);
  setMaxLogSize(50 * 1024);

  if (!initSPIFFS() || !loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[System] 配置加载失败，重启中...");
    ESP.restart();
  }
  printConfig(appConfig);

  if (!connectToWiFi(20000) || !multiNTPSetup(20000)) {
    Serial.println("[System] 网络连接失败，重启...");
    ESP.restart();
  }
  if (!connectToMQTT(20000)) {
    Serial.println("[System] MQTT连接失败，重启...");
    ESP.restart();
  }

  getMQTTClient().setCallback(mqttCallback);
  getMQTTClient().subscribe(appConfig.mqttResponseTopic.c_str());
  Serial.println("[MQTT] 已订阅：" + appConfig.mqttResponseTopic);

  if (!initSensorAndPump(25, 26, Serial1, 16, 17, 5000)) {
    Serial.println("[System] 传感器初始化失败，重启...");
    ESP.restart();
  }

  readMHZ16();  // 丢弃首次读数
  delay(500);

  // ========== 设置首次测量时间基准 ==========
  if (preferences.begin(NVS_NAMESPACE, false)) {
    unsigned long lastSec = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    time_t nowSec = time(nullptr);
    unsigned long intervalSec = appConfig.readInterval / 1000UL;
    unsigned long elapsedSec = (nowSec > lastSec) ? nowSec - lastSec : 0;

    if (elapsedSec >= intervalSec) {
      prevMeasureMs = millis() - appConfig.readInterval;
      Serial.println("[Init] 上次测量已过期，首次任务将立即执行");
    }
    else {
      unsigned long remainMs = (intervalSec - elapsedSec) * 1000UL;
      prevMeasureMs = millis() - (appConfig.readInterval - remainMs);
      Serial.printf("[Init] 距下次测量还有 %lu 秒\n", (intervalSec - elapsedSec));
    }
    preferences.end();
  }
  else {
    prevMeasureMs = millis() - appConfig.readInterval;
    Serial.println("[Init] 未读取到上次测量时间，立即执行首次测量");
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
  xTaskCreatePinnedToCore(commandTask, "CommandTask", 8192, NULL, 1, NULL, 1);

  Serial.println("[System] 初始化完成，进入主循环");
}

// ========== 主循环：维持 MQTT 心跳 ==========
void loop() {
  maintainMQTT(5000);
  delay(100);
}
