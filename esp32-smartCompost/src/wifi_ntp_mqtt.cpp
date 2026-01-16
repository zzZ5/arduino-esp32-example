// wifi_ntp_mqtt.cpp
#include "wifi_ntp_mqtt.h"
#include "config_manager.h"
#include <WiFi.h>
#include <time.h>

// 全局 WiFiClient 与 MQTT 客户端
static WiFiClient espClient;
PubSubClient mqttClient(espClient);

// WiFi 保活状态
static unsigned long lastWiFiCheck = 0;
static const unsigned long WIFI_CHECK_INTERVAL = 30000;  // 30秒检查一次

// WiFi 连接失败计数器
static int wifiFailCount = 0;
static const int WIFI_FAIL_LIMIT = 5;  // 连续失败5次后重启

// 外部访问 MQTT 客户端引用
PubSubClient& getMQTTClient() {
	return mqttClient;
}

/**
 * @brief 保持 WiFi 在线（自动重连）
 */
static void maintainWiFi() {
	unsigned long now = millis();
	if (now - lastWiFiCheck < WIFI_CHECK_INTERVAL) {
		return;
	}
	lastWiFiCheck = now;

	if (WiFi.status() != WL_CONNECTED) {
		Serial.println("[WiFi] Disconnected, reconnecting...");
		WiFi.disconnect();
		delay(500);
		if (!connectToWiFi(10000)) {
			Serial.println("[WiFi] Reconnect failed");
			wifiFailCount++;
			Serial.printf("[WiFi] Fail count: %d/%d\n", wifiFailCount, WIFI_FAIL_LIMIT);

			// 连续失败超过限制，重启设备
			if (wifiFailCount >= WIFI_FAIL_LIMIT) {
				Serial.println("[WiFi] Too many failures, restarting device...");
				delay(1000);
				ESP.restart();
			}
		} else {
			// 连接成功，重置计数器
			wifiFailCount = 0;
		}
	}
}

/**
 * @brief 连接 WiFi
 */
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
			return false;
		}
	}

	Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
	return true;
}

/**
 * @brief 等待 NTP 时间同步（每次尝试最多 waitMs 毫秒）
 */
static bool waitForSync(unsigned long waitMs) {
	unsigned long start = millis();
	struct tm tinfo;
	while ((millis() - start) < waitMs) {
		if (getLocalTime(&tinfo)) {
			return true;
		}
		delay(100);
	}
	return false;
}

/**
 * @brief 多 NTP 服务器同步方案，超时退出
 */
bool multiNTPSetup(unsigned long totalTimeoutMs) {
	unsigned long start = millis();
	bool synced = false;

	while (!synced) {
		for (const auto& server : appConfig.ntpServers) {
			if (millis() - start > totalTimeoutMs) {
				Serial.println("[NTP] overall timeout!");
				return false;
			}

			if (server.length() < 1) continue;

			Serial.print("[NTP] Trying server: ");
			Serial.println(server);

			configTime(0, 0, server.c_str());
			if (waitForSync(3000)) {
				Serial.println("[NTP] Success!");
				synced = true;
				break;
			}
			else {
				Serial.println("[NTP] Failed, try next...");
			}
		}

		if (!synced) {
			if (millis() - start > totalTimeoutMs) {
				Serial.println("[NTP] overall timeout (retry)");
				return false;
			}
			Serial.println("[NTP] All failed, retry after 2s...");
			delay(2000);
		}
	}

	// 设置时区：东八区
	configTime(8 * 3600, 0, appConfig.ntpServers[0].c_str());
	Serial.println("[NTP] Timezone set to UTC+8");
	return true;
}

/**
 * @brief 获取当前时间的字符串格式
 * 如果 NTP 未同步，返回默认时间字符串
 */
String getTimeString() {
	struct tm tinfo;
	if (!getLocalTime(&tinfo)) {
		// NTP 未同步，返回默认时间
		return "1970-01-01 00:00:00";
	}

	// 检查时间是否有效（1970 年表示未同步）
	if (tinfo.tm_year < 120) {  // 2020年以前认为未同步
		return "1970-01-01 00:00:00";
	}

	char buf[20];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tinfo);
	return String(buf);
}

/**
 * @brief 连接 MQTT 服务器
 */
bool connectToMQTT(unsigned long timeoutMs) {
	mqttClient.setServer(appConfig.mqttServer.c_str(), appConfig.mqttPort);
	mqttClient.setBufferSize(1024);

	unsigned long start = millis();
	while (!mqttClient.connected()) {
		if (WiFi.status() != WL_CONNECTED) {
			Serial.println("[MQTT] WiFi not connected, reconnecting...");
			if (!connectToWiFi(timeoutMs)) return false;
		}

		if (millis() - start > timeoutMs) {
			Serial.printf("[MQTT] connect timeout (> %lu ms)\n", timeoutMs);
			return false;
		}

		Serial.printf("[MQTT] Connecting to %s:%d...\n", appConfig.mqttServer.c_str(), appConfig.mqttPort);
		if (mqttClient.connect(appConfig.mqttClientId.c_str(),
			appConfig.mqttUser.c_str(),
			appConfig.mqttPass.c_str())) {
			Serial.println("[MQTT] Connected.");

			// 连接成功后重新订阅响应 topic
			String respTopic = appConfig.mqttResponseTopic();
			if (respTopic.length() > 0) {
				if (mqttClient.subscribe(respTopic.c_str())) {
					Serial.println("[MQTT] Resubscribed to response topic.");
				}
				else {
					Serial.println("[MQTT] Failed to subscribe response topic.");
				}
			}

			return true;
		}
		else {
			Serial.printf("[MQTT] Fail, state=%d. Retry in 300ms\n", mqttClient.state());
			delay(300);
		}
	}

	return false;
}

/**
 * @brief 保持 MQTT 在线（WiFi保活 + 重连 + loop）
 */
void maintainMQTT(unsigned long timeoutMs) {
	// 先保持 WiFi 在线
	maintainWiFi();

	if (!mqttClient.connected()) {
		Serial.println("[MQTT] Not connected, reconnecting...");
		connectToMQTT(timeoutMs);
	}
	mqttClient.loop();
}

/**
 * @brief 通过 MQTT 发布数据
 */
bool publishData(const String& topic, const String& payload, unsigned long timeoutMs) {
	unsigned long start = millis();

	// 先确保 WiFi 在线
	maintainWiFi();

	while (!mqttClient.connected()) {
		if (millis() - start > timeoutMs) {
			Serial.printf("[MQTT] publishData: connect timeout >%lu ms\n", timeoutMs);
			return false;
		}
		maintainWiFi();
		connectToMQTT(timeoutMs - (millis() - start));
	}

	while (millis() - start < timeoutMs) {
		if (mqttClient.publish(topic.c_str(), payload.c_str())) {
			Serial.println("[MQTT] Publish success:");
			Serial.println(payload);
			return true;
		}
		else {
			Serial.printf("[MQTT] Publish fail, state=%d. Retry in 300ms\n", mqttClient.state());
			delay(300);

			// 检查 WiFi 和 MQTT 连接
			maintainWiFi();
			if (!mqttClient.connected()) {
				connectToMQTT(timeoutMs - (millis() - start));
			}
		}
	}

	Serial.printf("[MQTT] publishData: overall timeout >%lu ms\n", timeoutMs);
	return false;
}
