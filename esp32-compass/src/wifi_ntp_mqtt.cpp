#include "wifi_ntp_mqtt.h"
#include "config_manager.h" // 需要访问appConfig
#include <WiFi.h>
#include <time.h>

// 全局 WiFiClient
static WiFiClient espClient;
PubSubClient mqttClient(espClient);

bool connectToWiFi(unsigned long timeoutMs) {
	WiFi.mode(WIFI_STA);
	WiFi.begin(appConfig.wifiSSID.c_str(), appConfig.wifiPass.c_str());

	Serial.print("[WiFi] Connecting to: ");
	Serial.println(appConfig.wifiSSID);

	unsigned long start = millis();
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		if (millis() - start > timeoutMs) {
			Serial.println("\n[WiFi] Timeout!");
			return false; // 表示失败
		}
	}
	Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
	return true;
}

static bool waitForSync(unsigned long ms) {
	unsigned long start = millis();
	struct tm tinfo;
	while ((millis() - start) < ms) {
		if (getLocalTime(&tinfo)) {
			return true;
		}
		delay(100);
	}
	return false;
}

bool multiNTPSetup(unsigned long totalTimeoutMs) {
	unsigned long start = millis();
	bool synced = false;
  
	while (!synced) {
	  // 依次尝试 3 台服务器
	  for (int i = 0; i < 3; i++) {
		// 若总耗时超时 => return false
		if (millis() - start > totalTimeoutMs) {
		  Serial.println("[NTP] overall timeout in multiNTPSetup!");
		  return false;
		}
  
		String server = appConfig.ntpServers[i];
		if (server.length() < 1) continue; // 若为空,跳过
		Serial.print("[NTP] configTime: ");
		Serial.println(server);
  
		// 先设置0时区
		configTime(0, 0, server.c_str());
		// 等待 5秒看能否同步
		if (waitForSync(5000)) {
		  Serial.println("[NTP] success!");
		  synced = true;
		  break;
		} else {
		  Serial.println("[NTP] fail, next...");
		}
	  }
  
	  if (!synced) {
		// 全部3台都失败 => 等2s再重试
		// 但要再次检查总超时
		if (millis() - start > totalTimeoutMs) {
		  Serial.println("[NTP] overall timeout in multiNTPSetup(2)!");
		  return false;
		}
		Serial.println("[NTP] all fail, wait 2s & retry...");
		delay(2000);
	  }
	}
  
	// 若成功 => 设置本地时区(UTC+8)
	configTime(8 * 3600, 0, appConfig.ntpServers[0].c_str());
	Serial.println("[NTP] done!");
	return true;
  }

// 获取本地时间字符串
// (前提：已通过多NTPSetup设置了本地时间)
String getTimeString() {
	struct tm tinfo;
	if (!getLocalTime(&tinfo)) {
		return "1970-01-01 00:00:00";
	}
	char buf[20];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tinfo);
	return String(buf);
}

/**
 * 短循环 + 超时检查 版本
 * 若在 timeoutMs 时间内无法连接到 MQTT，则返回 false
 */
bool connectToMQTT(unsigned long timeoutMs) {
	mqttClient.setServer(appConfig.mqttServer.c_str(), appConfig.mqttPort);
	mqttClient.setBufferSize(512);

	unsigned long start = millis();
	while (!mqttClient.connected()) {
		// 检测wifi是否连接
		if (WiFi.status() != WL_CONNECTED) {
			Serial.println("[MQTT] WiFi not connected, trying to reconnect WiFi...");
			if (!connectToWiFi(timeoutMs)) { 
			  Serial.println("[MQTT] Reconnect WiFi failed!");
			  return false;
			}
			else {
			  Serial.println("[MQTT] WiFi reconnected.");
			}
		}
		
		// 检测是否超时
		if (millis() - start > timeoutMs) {
			Serial.printf("[MQTT] connectToMQTT() timed out (> %lu ms)!\n", timeoutMs);
			return false; // 返回 false 让上层决定后续(重启等)
		}

		Serial.printf("[MQTT] Trying to connect %s:%d...\n",
			appConfig.mqttServer.c_str(),
			appConfig.mqttPort);
		if (mqttClient.connect(appConfig.mqttClientId.c_str(),
			appConfig.mqttUser.c_str(),
			appConfig.mqttPass.c_str())) {
			Serial.println("[MQTT] connected!");
			return true;
		}
		else {
			Serial.printf("[MQTT] fail, state=%d, wait 300ms\n", mqttClient.state());
			delay(300); // 小延时后再试
		}
	}
	return true; // 理论上不会到达此行
}

// 保持在线
void maintainMQTT(unsigned long timeoutMs) {
	if (!mqttClient.connected()) {
		connectToMQTT(timeoutMs);
	}
	mqttClient.loop();
}

/**
 * 短循环 + 超时检查 版本
 * 在 timeoutMs 时间内若无法成功 publish，就返回 false
 */
bool publishData(const String &topic, const String &payload, unsigned long timeoutMs) {
	unsigned long start = millis();
  
	// 1) 若未连接则尝试在 timeoutMs 内连上
	while (!mqttClient.connected()) {
	  if (millis() - start > timeoutMs) {
		Serial.printf("[MQTT] publishData(): connect timeout >%lu ms\n", timeoutMs);
		return false; 
	  }
	  if (!connectToMQTT(timeoutMs - (millis() - start))) {
		// connectToMQTT() 也可能失败或超时
		if (!mqttClient.connected()) {
		  Serial.println("[MQTT] Still not connected, publishData fail!");
		  return false;
		}
	  }
	}
	
	// 2) 执行 publish
	while(true) {
	  if (mqttClient.publish(topic.c_str(), payload.c_str())) {
		Serial.println("[MQTT] Publish success: " + payload);
		return true;
	  } else {
		// 若超时
		if (millis() - start > timeoutMs) {
		  Serial.printf("[MQTT] publishData() timed out(>%lu ms)!\n", timeoutMs);
		  return false;
		}
  
		Serial.printf("[MQTT] publish fail, state=%d, wait 300ms\n", mqttClient.state());
		delay(300);
  
		// 断线则再试连
		if (!mqttClient.connected()) {
		  if (!connectToMQTT(timeoutMs - (millis() - start))) {
			return false;
		  }
		}
	  }
	}
  }
  