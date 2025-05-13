#include <Arduino.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>  // 引入 ArduinoJson 库，用于生成 JSON 格式的消息

// MQTT 配置
const char* mqttServer = "118.25.108.254";  // MQTT 服务器地址
const int mqttPort = 1883;                  // MQTT 服务器端口
const char* mqttClientId = "mqttx_31";   // MQTT 客户端 ID
const char* mqttUsername = "test";          // MQTT 账号
const char* mqttPassword = "12345678";     // MQTT 密码
const char* subscribeTopic = "compostlab/test/response"; // 订阅主题
const char* publishTopic = "compostlab/test/post";       // 发布主题

// 串口设置和缓冲区
HardwareSerial ec800kSerial(1); // 使用 ESP32 的 UART1
#define BUFFER_SIZE 512
char buffer[BUFFER_SIZE];
volatile int bufferIndex = 0;
volatile bool responseComplete = false;

// 超时相关变量
unsigned long lastPublishTime = 0;
const unsigned long publishInterval = 60000; // 每隔 1 分钟发布一次消息
unsigned long lastReceiveRequestTime = 0;

// 函数声明
String sendATCommand(String command);
String queryNetworkTime();
void publishCurrentTime();
void requestReceiveMessage();

void setup() {
	// 初始化主机与 ESP32 的串口通信
	Serial.begin(115200);  // 调试用串口

	// 初始化 EC800K 的硬件串口
	ec800kSerial.begin(115200, SERIAL_8N1, 16, 17);  // RX, TX 引脚配置
	Serial.println("ESP32 与 EC800K MQTT 通信初始化完成！");

	// 在进行其他操作前，先进行 AT 命令测试，直到收到 OK
	bool atTestSuccess = false;
	for (int i = 0; i < 5; i++) {  // 最多重试 5 次
		String response = sendATCommand("AT");  // 发送 AT 命令
		if (response.indexOf("OK") >= 0) {
			atTestSuccess = true;
			break;  // 如果收到 OK，则跳出循环
		}
		delay(1000);  // 延迟 1 秒后重试
	}

	if (!atTestSuccess) {
		Serial.println("AT 命令测试失败，重启设备！");
		ESP.restart();  // 如果 5 次都没有收到 OK，则重启设备
	}

	// 初始化 MQTT 连接
	sendATCommand("AT+QMTDISC=0");  // 先关闭当前连接
	sendATCommand("AT+QMTOPEN=0,\"" + String(mqttServer) + "\"," + String(mqttPort));  // 打开连接到 MQTT 服务器
	delay(2000); // 等待连接
	sendATCommand("AT+QMTCONN=0,\"" + String(mqttClientId) + "\",\"" + String(mqttUsername) + "\",\"" + String(mqttPassword) + "\""); // 连接到 MQTT 服务器
	delay(2000); // 等待连接

	// 订阅主题 compostlab/test/response
	sendATCommand("AT+QMTSUB=0,1,\"" + String(subscribeTopic) + "\",1"); // 订阅 compostlab/test/response 主题
	delay(1000); // 等待订阅成功
}

void loop() {
	// 每隔 1 分钟发布当前时间
	unsigned long currentMillis = millis();
	if (currentMillis - lastPublishTime >= publishInterval) {
		publishCurrentTime();  // 发布当前时间
		lastPublishTime = currentMillis;  // 更新最后发布时间
	}

	// 主循环其他逻辑可在此添加
	delay(100); // 简单延时
}

// 发送 AT 命令到 EC800K，返回响应内容
String sendATCommand(String command) {
	command.trim();
	command += "\r\n";
	ec800kSerial.print(command);  // 发送到 EC800K
	Serial.print("发送命令: ");
	Serial.println(command);

	unsigned long startMillis = millis();
	String response = "";

	// 等待接收到响应
	while (millis() - startMillis < 10000) {
		if (ec800kSerial.available()) {
			char incomingByte = ec800kSerial.read();  // 读取响应
			response += incomingByte;  // 追加响应内容
		}

		// 检查是否收到完整的响应（即包括 OK\r\n 或 ERROR\r\n）
		if (response.indexOf("OK") >= 0 || response.indexOf("ERROR") >= 0) {
			break;  // 如果收到完整的响应，退出循环
		}
	}

	// 如果超时或未收到 OK 或 ERROR，返回空字符串
	if (response.indexOf("OK") < 0 && response.indexOf("ERROR") < 0) {
		Serial.println("未收到有效响应，超时或错误！");
		return "";
	}

	// 返回完整的响应内容
	Serial.print("收到的响应: ");
	Serial.println(response);
	return response;
}

// 发布当前时间的 JSON 消息
void publishCurrentTime() {
	// 创建 JSON 对象
	StaticJsonDocument<200> doc;
	doc["timestamp"] = queryNetworkTime();  // 当前时间，单位为毫秒
	doc["message"] = "time";

	// 序列化 JSON 为字符串
	String jsonString;
	serializeJson(doc, jsonString);

	// 发布到 MQTT 主题 compostlab/test/post
	sendATCommand("AT+QMTPUBEX=0,0,0,0,\"" + String(publishTopic) + "\"," + String(jsonString.length() + 2));
	sendATCommand(jsonString);
	Serial.print("time: ");
	Serial.println(jsonString);
}

// 查询网络时间
String queryNetworkTime() {
	String response = sendATCommand("AT+QLTS\r\n");  // 发送 AT+QLTS 命令
	if (response.indexOf("+QLTS:") >= 0) {
		// 提取时间字符串部分
		int startIdx = response.indexOf("\"") + 1;
		int endIdx = response.indexOf("\"", startIdx);
		if (startIdx >= 0 && endIdx > startIdx) {
			return response.substring(startIdx, endIdx);  // 返回网络时间字符串
		}
	}
	return "";  // 返回空字符串表示未能获取时间
}