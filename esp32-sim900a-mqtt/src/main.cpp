#define TINY_GSM_MODEM_SIM900  // 定义 GSM 调制解调器的型号为 SIM900

#include <TinyGsmClient.h>
#include <PubSubClient.h>

// 使用 UART1 作为 SIM900A 的串口通信
#define SerialMon Serial
#define SerialAT Serial1

// GPIO 引脚定义
#define MODEM_TX 17  // TX 引脚连接到 SIM900A 的 RX
#define MODEM_RX 16  // RX 引脚连接到 SIM900A 的 TX

// 中国移动的 APN 信息
const char apn[] = "cmnet";  // 中国移动的 APN
const char gprsUser[] = "";  // 不需要用户名
const char gprsPass[] = "";  // 不需要密码

// MQTT 信息
const char* broker = "118.25.108.254"; // MQTT 代理服务器 IP 地址
const char* mqttUser = "test";         // MQTT 用户名
const char* mqttPassword = "12345678"; // MQTT 密码
const char* topicSub = "compostlab/test"; // 要订阅的主题
const char* topicPub = "compostlab/test"; // 要发布的主题

TinyGsm modem(SerialAT);
TinyGsmClient client(modem);
PubSubClient mqtt(client);

// 方法声明
void initializeModem();
void connectToGPRS();
void connectToMQTT();
void publishData(const char* message);

// 回调函数，用于处理接收到的 MQTT 消息
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  SerialMon.print("Message arrived [");
  SerialMon.print(topic);
  SerialMon.print("]: ");
  for (int i = 0; i < length; i++) {
    SerialMon.print((char)payload[i]);
  }
  SerialMon.println();
}

void setup() {
  // 初始化串口监视器
  SerialMon.begin(115200);
  delay(10);

  // 初始化调制解调器
  initializeModem();

  // 连接 GPRS 网络
  connectToGPRS();

  // 设置 MQTT 回调函数
  mqtt.setCallback(mqttCallback);

  // 连接到 MQTT 服务器
  connectToMQTT();
}

void loop() {
  // 检查并维持 MQTT 连接
  if (!mqtt.connected()) {
    connectToMQTT();
  }

  mqtt.loop();  // 保持 MQTT 客户端运行以接收消息

  // 定期发布数据
  static unsigned long lastPublishTime = 0;
  unsigned long currentMillis = millis();
  if (currentMillis - lastPublishTime >= 5000) { // 每 5 秒发布一次数据
    lastPublishTime = currentMillis;
    publishData("Hello from ESP32 and SIM900A!");
  }
}

// 方法定义：初始化调制解调器
void initializeModem() {
  // 初始化 SIM900A 的串口，使用 UART1 引脚 TX=17, RX=16
  SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(3000);

  // 启动调制解调器
  SerialMon.println("Initializing modem...");
  modem.restart();
}

// 方法定义：连接 GPRS 网络
void connectToGPRS() {
  SerialMon.print("Connecting to APN: ");
  SerialMon.println(apn);
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    SerialMon.println("Failed to connect to GPRS");
    while (true);
  }
  SerialMon.println("GPRS connected");
}

// 方法定义：连接到 MQTT 服务器
void connectToMQTT() {
  SerialMon.print("Connecting to MQTT broker: ");
  SerialMon.println(broker);
  while (!mqtt.connect("ESP32_SIM900", mqttUser, mqttPassword)) {
    SerialMon.print(".");
    delay(1000);
  }
  SerialMon.println("\nConnected to MQTT broker");

  // 订阅主题
  mqtt.subscribe(topicSub);
  SerialMon.print("Subscribed to topic: ");
  SerialMon.println(topicSub);
}

// 方法定义：发布数据到指定主题
void publishData(const char* message) {
  SerialMon.print("Publishing to topic: ");
  SerialMon.println(topicPub);
  if (mqtt.publish(topicPub, message)) {
    SerialMon.println("Publish succeeded");
  }
  else {
    SerialMon.println("Publish failed");
  }
}
