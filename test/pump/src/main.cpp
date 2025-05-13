/************************************************************
 * 结合四合一气体传感器 + 气泵 + Wi-Fi + MQTT 上传示例
 *
 * 功能说明：
 *  1. 每隔一定周期(READ_INTERVAL)执行数据采集。
 *  2. 采集前先开启气泵(PUMP_PIN)运行1分钟，让传感器吸入空气。
 *  3. 发送命令读取四合一传感器数据，计算得到 CO/H2S/O2/CH4。
 *  4. 关闭气泵，通过MQTT发布数据到服务器。
 ************************************************************/

#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>

 //======== 1) Wi-Fi 配置 ========//
const char* ssid = "zzZ5";      // 修改为实际Wi-Fi名
const char* password = "1450791278";  // 修改为Wi-Fi密码

//======== 2) MQTT 配置 ========//
const char* mqttServer = "118.25.108.254";  // 示例公共测试服务器，替换为您自己的Broker
const int   mqttPort = 1883;
// 如果服务器需要用户名密码，请取消注释并填入
const char* mqttUser = "equipment";
const char* mqttPass = "ZNXK8888";

const char* mqttClientId = "linhu";     // MQTT 客户端ID，需保证唯一
const char* mqttTopic = "compostlab/test/post";       // 发布消息的主题

//======== 3) 氣泵 & 采集周期设定 ========//
const int PUMP_PIN = 4;                     // 用于控制气泵的引脚
const unsigned long READ_INTERVAL = 300000; // 每5分钟采集一次(单位ms)
unsigned long previousTime = 0;             // 记录上次采集时间

// 硬件串口 (传感器)
HardwareSerial mySerial(1);
#define RX_PIN 16
#define TX_PIN 17

WiFiClient espClient;
PubSubClient mqttClient(espClient);

//============================================================
//               1)  氣泵控制函数封装
//============================================================
void pumpOn() {
  digitalWrite(PUMP_PIN, LOW);
  Serial.println("Pump ON");
}

void pumpOff() {
  digitalWrite(PUMP_PIN, HIGH);
  Serial.println("Pump OFF");
}

//============================================================
//        2)  传感器初始化 / 读取函数 (示例)
//============================================================

// 传感器初始化：切换到问答模式
void initializeSensor() {
  Serial.println("初始化传感器 -> 切换问答模式...");
  // 此处发送传感器特定命令
  uint8_t switchToQueryMode[] = { 0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x46 };
  mySerial.write(switchToQueryMode, sizeof(switchToQueryMode));
  delay(1000);
}

// 读取四合一传感器 (示例)
bool readSensorData(uint16_t& coVal,
  uint16_t& h2sVal,
  float& o2Val,
  uint16_t& ch4Val)
{
  // 发送查询命令
  uint8_t queryCommand[] = { 0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79 };
  mySerial.write(queryCommand, sizeof(queryCommand));

  // 接收 11 字节响应
  uint8_t response[11];
  unsigned long startTime = millis();
  uint8_t index = 0;
  while (index < 11 && (millis() - startTime) < 200) {
    if (mySerial.available()) {
      response[index++] = mySerial.read();
    }
  }
  // 判断是否读满 11 字节
  if (index < 11) {
    Serial.println("数据帧读取不完整！");
    return false;
  }

  // 校验和
  uint8_t calcSum = 0;
  for (uint8_t i = 1; i < 10; i++) {
    calcSum += response[i];
  }
  calcSum = (~calcSum) + 1;
  if (calcSum != response[10]) {
    Serial.println("校验失败！");
    return false;
  }

  // 解析
  coVal = (response[2] << 8) | response[3];
  h2sVal = (response[4] << 8) | response[5];
  o2Val = ((response[6] << 8) | response[7]) * 0.1f;
  ch4Val = (response[8] << 8) | response[9];

  return true;
}

//============================================================
//       3)  Wi-Fi / MQTT 相关函数 (简化示例)
//============================================================
void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("连接Wi-Fi: ");
  Serial.println(ssid);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nWiFi 已连接, IP: ");
  Serial.println(WiFi.localIP());
}

void connectToMQTT() {
  mqttClient.setServer(mqttServer, mqttPort);
  Serial.println("连接 MQTT: " + String(mqttServer) + ":" + String(mqttPort));
  while (!mqttClient.connected()) {
    Serial.print("连接到 MQTT: ");
    if (mqttClient.connect("ESP32_GasSensor")) {
      Serial.println("成功!");
    }
    else {
      Serial.print("失败, 错误码: ");
      Serial.println(mqttClient.state());
      delay(2000);
    }
  }
}

void publishData(const String& payload) {
  if (mqttClient.publish(mqttTopic, payload.c_str())) {
    Serial.println("MQTT 发布成功: " + payload);
  }
  else {
    Serial.println("MQTT 发布失败!");
  }
}

//============================================================
//                 setup() / loop()
//============================================================
void setup() {
  Serial.begin(115200);

  // 氣泵引脚初始化
  pinMode(PUMP_PIN, OUTPUT);
  pumpOff();  // 初始关闭

  // 传感器串口初始化
  mySerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  initializeSensor();

  // 连接 Wi-Fi
  connectToWiFi();
  // 连接 MQTT
  connectToMQTT();

  Serial.println("Setup 完成. 进入主循环...");
}

void loop() {
  // 让 MQTT 客户端处理心跳
  if (!mqttClient.connected()) {
    connectToMQTT();
  }
  mqttClient.loop();

  // 每隔 5 分钟做一次传感器采样
  unsigned long currentTime = millis();
  if (currentTime - previousTime >= READ_INTERVAL) {
    previousTime = currentTime;

    //=== 1) 打开气泵 1分钟 ===
    pumpOn();
    delay(60000);

    //=== 2) 读取传感器 ===
    uint16_t coVal, h2sVal, ch4Val;
    float o2Val;
    if (readSensorData(coVal, h2sVal, o2Val, ch4Val)) {
      // 打印到串口
      Serial.printf("CO=%u ppm, H2S=%u ppm, O2=%.1f %%VOL, CH4=%u %%LEL\n",
        coVal, h2sVal, o2Val, ch4Val);

      // 组装JSON并发布
      String payload = "{";
      payload += "\"CO\":" + String(coVal) + ",";
      payload += "\"H2S\":" + String(h2sVal) + ",";
      payload += "\"O2\":" + String(o2Val, 1) + ",";
      payload += "\"CH4\":" + String(ch4Val);
      payload += "}";
      publishData(payload);
    }
    else {
      Serial.println("读取传感器数据失败!");
    }

    //=== 3) 关闭气泵 ===
    pumpOff();
  }
}