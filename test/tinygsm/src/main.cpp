#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// 定义硬件串口
#define SerialMon Serial
#define SerialAT ec800kSerial
#define TINY_GSM_MODEM_BG96  // 根据硬件型号选择

// MQTT 配置
const char* mqttServer = "118.25.108.254";
const int mqttPort = 1883;
const char* mqttClientId = "mqtt_379435";
const char* mqttUsername = "test";
const char* mqttPassword = "12345678";
const char* subscribeTopic = "compostlab/test/response";
const char* publishTopic = "compostlab/test/post";

// 选择硬件串口
HardwareSerial ec800kSerial(1); // 使用 ESP32 的 UART1

// TinyGsm 和 PubSubClient 对象
TinyGsm modem(SerialAT);    // EC800K 模块
TinyGsmClient gsmClient(modem);
PubSubClient client(gsmClient);

// 超时相关变量
unsigned long lastPublishTime = 0;
const unsigned long publishInterval = 60000; // 每隔 1 分钟发布一次消息（60000 毫秒）
unsigned long lastReceiveRequestTime = 0;    // 上次请求接收消息的时间
const unsigned long receiveInterval = 60000; // 每隔 1 分钟请求接收消息的时间

// 函数声明
void mqttCallback(char* topic, byte* payload, unsigned int length);
void connectToMQTT();
void publishCurrentTime();
void requestReceiveMessage();

void setup() {
  // 初始化串口和通信
  Serial.begin(115200);  // 调试用串口
  ec800kSerial.begin(115200, SERIAL_8N1, 16, 17);  // RX, TX 引脚配置

  Serial.println("ESP32 与 EC800K MQTT 通信初始化完成！");

  // 初始化 TinyGSM 模块
  modem.restart();
  modem.waitForNetwork();
  Serial.println("网络连接成功!");

  // 连接到 MQTT 服务器
  client.setServer(mqttServer, mqttPort);
  client.setCallback(mqttCallback);

  // 连接到 MQTT 服务器
  connectToMQTT();
}

void loop() {
  // 检查 MQTT 连接并保持活跃
  if (!client.connected()) {
    connectToMQTT();  // 如果没有连接，则尝试重新连接
  }
  client.loop();

  // 每隔 1 分钟发布当前时间
  unsigned long currentMillis = millis();
  if (currentMillis - lastPublishTime >= publishInterval) {
    publishCurrentTime();  // 发布当前时间
    lastPublishTime = currentMillis;  // 更新最后发布时间
  }

  // 每隔 1 分钟请求接收消息
  if (currentMillis - lastReceiveRequestTime >= receiveInterval) {
    requestReceiveMessage();  // 请求接收消息
    lastReceiveRequestTime = currentMillis;  // 更新上次请求接收消息的时间
  }

  // 主循环其他逻辑可在此添加
  delay(100); // 简单延时
}

// 连接到 MQTT 服务器
void connectToMQTT() {
  Serial.println("连接到 MQTT 服务器...");
  while (!client.connected()) {
    if (client.connect(mqttClientId, mqttUsername, mqttPassword)) {
      Serial.println("MQTT 连接成功!");
      // 订阅主题
      client.subscribe(subscribeTopic);
    }
    else {
      Serial.print("MQTT 连接失败, 状态: ");
      Serial.println(client.state());
      delay(5000);
    }
  }
}

// MQTT 消息回调函数
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.print("收到消息: ");
  Serial.println(message);
  // 处理接收到的消息
}

// 发布当前时间的 JSON 消息
void publishCurrentTime() {
  // 创建 JSON 对象
  StaticJsonDocument<200> doc;
  doc["timestamp"] = millis();  // 当前时间，单位为毫秒
  doc["message"] = "当前时间消息";

  // 序列化 JSON 为字符串
  String jsonString;
  serializeJson(doc, jsonString);

  // 计算消息的总长度
  int messageLength = jsonString.length() + 2; // +2 是因为 \r\n

  // 发送 MQTT 发布消息
  if (client.publish(publishTopic, jsonString.c_str())) {
    Serial.print("发布当前时间的消息: ");
    Serial.println(jsonString);
  }
  else {
    Serial.println("发布消息失败！");
  }
}

// 请求接收消息
void requestReceiveMessage() {
  Serial.println("请求接收消息...");
  // 使用 AT 命令请求接收消息
  String command = "AT+QMTRCV=0,1,1024"; // 假设 1024 是接收缓冲区的大小
  modem.sendAT(command);  // 发送 AT 命令请求
}
