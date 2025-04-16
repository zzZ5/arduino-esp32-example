#include <Arduino.h>
#include <Preferences.h>
#include <time.h> 
#include "log_manager.h"
#include "config_manager.h"
#include "wifi_ntp_mqtt.h"
#include "sensor.h"

// 全局 NVS对象
Preferences preferences;

// 命名空间 & key
static const char* NVS_NAMESPACE = "my-nvs";
static const char* NVS_KEY_LAST_MEAS = "lastMeas";

// 用于 loop 中再周期测量
static unsigned long prevMeasureMs = 0; // 记录 millis(), 用来在loop里定时

// 常量定义
// Wi-Fi 超时(示例20秒)
static const unsigned long WIFI_TIMEOUT = 20000UL;
// NTP 超时(示例: 20秒)
static const unsigned long NTP_TIMEOUT = 20000UL;
// MQTT 超时(示例20秒)
static const unsigned long MQTT_TIMEOUT = 20000UL;
// 初始化超时(示例5秒)
static const unsigned long INIT_TIMEOUT = 5000UL;

// 函数声明
bool doMeasurementAndSave();

// 进入轻度睡眠模式（节省电量）
void goToLightSleep() {
  Serial.println("Going to light sleep for 1 minute...");
  esp_sleep_enable_timer_wakeup(1 * 60 * 1000000);  // 设置1分钟后唤醒esp_sleep_enable_timer_wakeup11
  esp_light_sleep_start();  // ESP32将进入轻度睡眠状态，Wi-Fi模块仍然会保持活动
}

// 进入深度睡眠模式（节省电量，非必需，可根据需要调整）
void goToDeepSleep() {
  Serial.println("Going to deep sleep for 10 minutes...");
  esp_sleep_enable_timer_wakeup(10 * 60 * 500000);  // 设置为5分钟
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  Serial.println("[Setup] Program start...");

  //=== 1) 初始化日志系统(若尚未mount SPIFFS,可在 initLogSystem中做)
  if (!initLogSystem()) {
    Serial.println("[Setup] initLogSystem fail, proceed anyway...");
  }
  else {
    // 设置最小日志等级(示例：只写INFO以上)
    setMinLogLevel(LogLevel::INFO);
    // 设置日志文件最大50KB
    setMaxLogSize(50 * 1024);
    // 记录启动
    logWrite(LogLevel::INFO, "Device booting, log system ready.");
  }

  //=== 2) 挂载 SPIFFS 并加载配置
  if (!initSPIFFS()) {
    Serial.println("[Setup] SPIFFS init fail => reboot");
    logWrite(LogLevel::ERROR, "SPIFFS init fail => reboot");
    ESP.restart();
  }
  logWrite(LogLevel::INFO, "SPIFFS init OK");

  if (!loadConfigFromSPIFFS("/config.json")) {
    Serial.println("[Setup] no /config.json => use defaults");
    logWrite(LogLevel::WARN, "No config => use defaults");
  }
  else {
    logWrite(LogLevel::INFO, "Config loaded from /config.json");
  }
  printConfig(appConfig);


  //=== 3) Wi-Fi 连接
  logWrite(LogLevel::INFO, "Connecting WiFi...");
  unsigned long startTime = millis();
  if (!connectToWiFi(WIFI_TIMEOUT)) {
    logWrite(LogLevel::ERROR, "WiFi connect fail => reboot");
    ESP.restart();
  }
  logWrite(LogLevel::INFO, "WiFi connected.");

  //=== 4) NTP
  logWrite(LogLevel::INFO, "multiNTPSetup with 20s totalTimeout");
  startTime = millis();
  if (!multiNTPSetup(NTP_TIMEOUT)) {
    logWrite(LogLevel::ERROR, "NTP fail => reboot");
    ESP.restart();
  }
  logWrite(LogLevel::INFO, "NTP done.");

  //=== 5) MQTT连接
  logWrite(LogLevel::INFO, "Connect MQTT...");
  startTime = millis();
  if (!connectToMQTT(MQTT_TIMEOUT)) {
    logWrite(LogLevel::ERROR, "MQTT connect fail => reboot");
    ESP.restart();
  }
  logWrite(LogLevel::INFO, "MQTT connected OK");

  //=== 6) 初始化传感器 & 气泵
  logWrite(LogLevel::INFO, "Init sensor & pump with 5s timeout...");
  startTime = millis();
  if (!initSensorAndPump(4, Serial1, 16, 17, INIT_TIMEOUT)) {
    logWrite(LogLevel::ERROR, "initSensorAndPump fail => reboot");
    ESP.restart();
  }
  logWrite(LogLevel::INFO, "Sensor & pump inited.");


  //=== 7)   从NVS读取 lastMeasureTime, 计算是否需等待
  if (!preferences.begin(NVS_NAMESPACE, false)) {
    logWrite(LogLevel::ERROR, "Preferences begin fail => can't store lastMeas!");
  }
  else {
    unsigned long lastMeasSec = preferences.getULong(NVS_KEY_LAST_MEAS, 0);
    logWrite(LogLevel::INFO, String("NVS lastMeasureTime=") + lastMeasSec);

    // 当前epoch
    time_t nowEpoch = time(nullptr);
    if (nowEpoch < 1680000000UL) {
      logWrite(LogLevel::WARN, "NTP maybe not sync? nowEpoch too small...");
    }

    // 计算距离上次测量
    unsigned long intervalSec = appConfig.readInterval / 1000UL;
    unsigned long pumpRunSec = appConfig.pumpRunTime / 1000UL;
    unsigned long effectiveInterval = (intervalSec > pumpRunSec) ? (intervalSec - pumpRunSec) : 0;
    unsigned long elapsed = (nowEpoch > lastMeasSec) ? (nowEpoch - lastMeasSec) : 0;

    if (lastMeasSec == 0) {
      logWrite(LogLevel::INFO, "No recorded measure => do measure now");
    }
    else {
      if (elapsed < effectiveInterval) {
        unsigned long waitSec = effectiveInterval - elapsed;
        logWrite(LogLevel::INFO, String("Last measure was ") + elapsed
          + "s ago, wait " + waitSec + "s to next measure");
        delay(waitSec * 1000UL); // 阻塞等待
      }
      else {
        logWrite(LogLevel::INFO, "Interval passed => measure immediately");
      }
    }

    // 记录当前 millis, loop中继续定时
    prevMeasureMs = millis();
    // 做一次测量
    if (!doMeasurementAndSave()) {
      logWrite(LogLevel::ERROR, "Initial measure fail => reboot");
      ESP.restart();
    }
  }


  Serial.println("[Setup] All done, enter loop");
  logWrite(LogLevel::INFO, "Setup done, entering loop");

  // 进入轻度睡眠等待下一次测量
  goToLightSleep();  // 在每次采集后节省电量
}

void loop() {
  // 每 appConfig.readInterval 毫秒再测
  unsigned long nowMs = millis();
  if (nowMs - prevMeasureMs >= appConfig.readInterval) {
    prevMeasureMs = nowMs;
    if (!doMeasurementAndSave()) {
      logWrite(LogLevel::ERROR, "Loop measure fail => reboot");
      ESP.restart();
    }
  }

  delay(50);
  goToLightSleep();  // 在每次周期后进入轻度睡眠以节省电量
}



//==========================================================
// 执行一次测量 + 发布 => 若成功就更新 lastMeasureTime => NVS
//==========================================================
bool doMeasurementAndSave() {
  // 1) 打开气泵, 延时 appConfig.pumpRunTime (阻塞式)
  pumpOn();
  logWrite(LogLevel::INFO, "Pump ON, wait 60s...");
  delay(appConfig.pumpRunTime);
  pumpOff();
  logWrite(LogLevel::INFO, "Pump OFF, reading sensor...");

  // 2) 读传感器
  uint16_t coVal, h2sVal, ch4Val;
  float o2Val;
  if (!readFourInOneSensor(coVal, h2sVal, o2Val, ch4Val)) {
    logWrite(LogLevel::WARN, "Sensor read fail => skip publish");
    return false;
  }

  // 3) 拼装 JSON
  String measuredTime = getTimeString();
  time_t nowEpoch = time(nullptr);

  // 组装 payload
  String payload = "{";
  payload += "\"data\":[";
  // CO
  payload += "{";
  payload += "\"value\":" + String(coVal);
  payload += ",\"key\":\"" + appConfig.keyCO + "\"";        // 从 config 中读取 CO 的键
  payload += ",\"measured_time\":\"" + measuredTime + "\"";
  payload += "},";

  // H2S
  payload += "{";
  payload += "\"value\":" + String(h2sVal);
  payload += ",\"key\":\"" + appConfig.keyH2S + "\"";       // 从 config 中读取 H2S 的键
  payload += ",\"measured_time\":\"" + measuredTime + "\"";
  payload += "},";

  // O2
  payload += "{";
  payload += "\"value\":" + String(o2Val, 1);
  payload += ",\"key\":\"" + appConfig.keyO2 + "\"";        // 从 config 中读取 O2 的键
  payload += ",\"measured_time\":\"" + measuredTime + "\"";
  payload += "},";

  // CH4
  payload += "{";
  payload += "\"value\":" + String(ch4Val);
  payload += ",\"key\":\"" + appConfig.keyCH4 + "\"";       // 从 config 中读取 CH4 的键
  payload += ",\"measured_time\":\"" + measuredTime + "\"";
  payload += "}";

  payload += "]}";

  // 4) 发布
  logWrite(LogLevel::INFO, "Publishing...");
  if (!publishData(appConfig.mqttTopic, payload, MQTT_TIMEOUT)) {
    logWrite(LogLevel::ERROR, "publishData fail => no lastMeas update");
    return false;
  }
  logWrite(LogLevel::INFO, "Publish success => store lastMeasureTime in NVS");

  // 5) 更新 NVS
  preferences.putULong(NVS_KEY_LAST_MEAS, (unsigned long)nowEpoch);
  return true;
}