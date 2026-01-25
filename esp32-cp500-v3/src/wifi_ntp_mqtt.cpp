#include "wifi_ntp_mqtt.h"
#include "config_manager.h"
#include <WiFi.h>
#include <time.h>
#include <PubSubClient.h>
#include <ESP.h>

// 外部声明获取 topic 函数
extern String getTelemetryTopic();
extern String getResponseTopic();

// 全局 WiFiClient 与 MQTT 客户端
static WiFiClient espClient;
PubSubClient mqttClient(espClient);

// WiFi 和 MQTT 状态跟踪
static unsigned long lastWiFiCheckTime = 0;
static unsigned long lastMQTTPublishSuccess = 0;
static unsigned long lastWiFiConnectedTime = 0;
static unsigned long consecutiveWiFiFailures = 0;
static unsigned long consecutiveMQTTFailures = 0;
static const unsigned long MQTT_KEEPALIVE_INTERVAL = 60000; // MQTT心跳间隔(ms)
static const unsigned long MAX_CONSECUTIVE_FAILURES = 10;  // 最大连续失败次数

// 自动重启配置
static const unsigned long MAX_WIFI_FAILURES_FOR_RESTART = 30;   // WiFi连接失败30次后重启
static const unsigned long MAX_NETWORK_UNAVAILABLE_TIME = 600000; // WiFi已连接但无网超过10分钟重启
static bool networkUnavailableWarning = false;

// 外部访问 MQTT 客户端引用
PubSubClient& getMQTTClient() {
	return mqttClient;
}

/**
 * @brief 检查网络是否真正可用（通过DNS解析测试）
 * @return true 网络可用, false 网络不可用
 */
static bool checkNetworkAvailable() {
	// 尝试解析公共DNS服务器(中国优化)
	IPAddress dns1, dns2;

	// 使用异步DNS解析或手动超时控制
	unsigned long startTime = millis();
	int dnsResult = WiFi.hostByName("www.baidu.com", dns1);
	unsigned long elapsed = millis() - startTime;

	// 记录DNS解析耗时
	Serial.printf("[DNS] www.baidu.com resolve time: %lu ms, result: %d\n", elapsed, dnsResult);

	if (dnsResult == 1) {
		return true;
	}

	// 如果DNS解析耗时超过5秒,认为网络不可用
	if (elapsed > 5000) {
		Serial.printf("[DNS] Timeout (> 5 seconds)\n");
		return false;
	}

	// 备用测试: 使用其他公共DNS服务进行解析测试
	startTime = millis();
	dnsResult = WiFi.hostByName("www.aliyun.com", dns2);  // 阿里云DNS解析测试
	elapsed = millis() - startTime;

	Serial.printf("[DNS] www.aliyun.com resolve time: %lu ms, result: %d\n", elapsed, dnsResult);
	return (dnsResult == 1);
}

/**
 * @brief 系统安全重启函数
 * @param reason 重启原因
 */
static void safeRestart(const char* reason) {
	Serial.println("========================================");
	Serial.printf("[RESTART] %s\n", reason);
	Serial.println("[RESTART] System will restart in 3 seconds...");
	Serial.println("========================================");

	// 断开MQTT和WiFi
	mqttClient.disconnect();
	WiFi.disconnect();

	delay(3000);
	ESP.restart();
}

/**
 * @brief 检查并触发自动重启
 */
static void checkAutoRestart() {
	unsigned long now = millis();

	// 检查WiFi连接失败次数是否过多
	if (consecutiveWiFiFailures >= MAX_WIFI_FAILURES_FOR_RESTART) {
		Serial.printf("[AUTO-RESTART] WiFi failed %lu times, triggering restart\n", consecutiveWiFiFailures);
		safeRestart("Too many WiFi connection failures");
		return;
	}

	// 检查WiFi已连接但网络不可用的时间
	if (WiFi.status() == WL_CONNECTED && lastWiFiConnectedTime > 0) {
		if (!checkNetworkAvailable()) {
			unsigned long unavailableTime = now - lastWiFiConnectedTime;

			if (unavailableTime > MAX_NETWORK_UNAVAILABLE_TIME) {
				Serial.printf("[AUTO-RESTART] WiFi connected but no internet for %lu seconds\n",
					unavailableTime / 1000);
				safeRestart("WiFi connected but no internet");
				return;
			}
			else if (unavailableTime > MAX_NETWORK_UNAVAILABLE_TIME / 2 && !networkUnavailableWarning) {
				networkUnavailableWarning = true;
				Serial.printf("[WARNING] WiFi connected but no internet for %lu seconds, will restart at %lu seconds\n",
					unavailableTime / 1000,
					MAX_NETWORK_UNAVAILABLE_TIME / 1000);
			}
		} else {
			// 网络可用,重置警告
			networkUnavailableWarning = false;
		}
	}
}

/**
 * @brief 连接 WiFi（带断线自动重连）
 */
bool connectToWiFi(unsigned long timeoutMs) {
	// 如果已经连接，快速检查网络是否真正可用
	if (WiFi.status() == WL_CONNECTED) {
		// 尝试ping一下网关（简单检查网络可达性）
		if (WiFi.gatewayIP().toString() != "0.0.0.0") {
			return true;
		}
	}

	// 断开旧连接，重新连接
	WiFi.disconnect();
	delay(100);
	WiFi.mode(WIFI_STA);
	WiFi.setAutoReconnect(true);  // 启用自动重连
	WiFi.setSleep(false);           // 禁用WiFi休眠，保持连接
	WiFi.begin(appConfig.wifiSSID.c_str(), appConfig.wifiPass.c_str());

	Serial.print("[WiFi] Connecting to: ");
	Serial.println(appConfig.wifiSSID);

	unsigned long start = millis();
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		Serial.print(".");
		if (millis() - start > timeoutMs) {
			Serial.println("\n[WiFi] Timeout!");
			consecutiveWiFiFailures++;
			Serial.printf("[WiFi] Consecutive failures: %lu/%lu\n",
				consecutiveWiFiFailures, MAX_WIFI_FAILURES_FOR_RESTART);
			return false;
		}
	}

	Serial.printf("\n[WiFi] Connected, IP: %s, RSSI: %d dBm\n",
		WiFi.localIP().toString().c_str(), WiFi.RSSI());

	// WiFi连接成功，记录时间和重置失败计数
	lastWiFiConnectedTime = millis();
	consecutiveWiFiFailures = 0;
	networkUnavailableWarning = false;

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

	// 验证时间是否有效（确保时区设置后时间仍然有效）
	struct tm tinfo;
	if (getLocalTime(&tinfo)) {
		Serial.println("[NTP] Timezone set to UTC+8, time validated");
		Serial.printf("[NTP] Current time: %04d-%02d-%02d %02d:%02d:%02d\n",
			tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday,
			tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec);
		return true;
	} else {
		Serial.println("[NTP] Timezone set but time validation failed!");
		return false;
	}
}

/**
 * @brief 获取当前时间的字符串格式
 */
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
 * @brief 获取公网IP地址
 * @return 公网IP字符串，失败则返回局域网IP
 */
String getPublicIP() {
	// 优先返回局域网IP作为备份
	String localIP = WiFi.localIP().toString();

	// 多个公网IP查询服务
	const char* services[] = {
		"ifconfig.me",
		"icanhazip.com",
		"ipecho.net/plain",
		"api.ipify.org"
	};
	const size_t numServices = sizeof(services) / sizeof(services[0]);

	Serial.println("[PublicIP] Attempting to fetch public IP...");

	for (size_t i = 0; i < numServices; i++) {
		WiFiClient client;
		const char* host = services[i];
		int port = 80;  // HTTP端口

		Serial.printf("[PublicIP] Trying service %d: %s\n", i + 1, host);

		unsigned long start = millis();
		if (client.connect(host, port)) {
			// 发送HTTP GET请求
			client.println("GET / HTTP/1.1");
			client.print("Host: ");
			client.println(host);
			client.println("Connection: close");
			client.println();

			// 读取响应，超时5秒
			String ip = "";
			unsigned long timeoutStart = millis();
			while (client.connected() && (millis() - timeoutStart < 5000)) {
				if (client.available()) {
					char c = client.read();
					if (c != '\r' && c != '\n') {
						ip += c;
					}
					// 如果已经读取到足够的数据，提前结束
					if (ip.length() > 15) {  // IPv4最长15字符
						break;
					}
				}
				delay(10);
			}
			client.stop();

			unsigned long elapsed = millis() - start;
			Serial.printf("[PublicIP] Service %d: elapsed=%lu ms, result=%s\n",
				i + 1, elapsed, ip.c_str());

			// 验证IP格式（简单验证）
			if (ip.length() >= 7 && ip.length() <= 15) {
				// 检查是否包含3个点
				int dotCount = 0;
				for (size_t j = 0; j < ip.length(); j++) {
					if (ip[j] == '.') dotCount++;
				}
				if (dotCount == 3) {
					Serial.printf("[PublicIP] ✓ Public IP obtained: %s\n", ip.c_str());
					return ip;
				}
			}
		} else {
			Serial.printf("[PublicIP] Service %d: connection failed\n", i + 1);
		}
		client.stop();
	}

	// 所有服务都失败，返回局域网IP
	Serial.printf("[PublicIP] ✗ All services failed, returning local IP: %s\n", localIP.c_str());
	return localIP;
}

/**
 * @brief 连接 MQTT 服务器（带指数退避重试）
 */
bool connectToMQTT(unsigned long timeoutMs) {
	mqttClient.setServer(appConfig.mqttServer.c_str(), appConfig.mqttPort);
	mqttClient.setBufferSize(4096);  // 增大缓冲区到4KB，防止消息截断

	unsigned long start = millis();
	unsigned long retryDelay = 500;  // 初始重试延迟
	int retryCount = 0;

	while (!mqttClient.connected()) {
		// 检查WiFi状态
		if (WiFi.status() != WL_CONNECTED) {
			Serial.println("[MQTT] WiFi not connected, reconnecting...");
			if (!connectToWiFi(15000)) {
				Serial.println("[MQTT] WiFi reconnect failed, wait before retry...");
				delay(5000);
				continue;
			}
		}

		if (millis() - start > timeoutMs) {
			Serial.printf("[MQTT] connect timeout (> %lu ms)\n", timeoutMs);
			return false;
		}

		String clientId = "cp500_" + appConfig.mqttDeviceCode + "_" + String(millis() % 10000);
		Serial.printf("[MQTT] Connecting to %s:%d as %s... (attempt %d)\n",
			appConfig.mqttServer.c_str(), appConfig.mqttPort, clientId.c_str(), ++retryCount);

		if (mqttClient.connect(clientId.c_str(),
			appConfig.mqttUser.c_str(),
			appConfig.mqttPass.c_str())) {
			Serial.println("[MQTT] Connected.");

			// 连接成功后重新订阅响应 topic
			String responseTopic = getResponseTopic();
			if (responseTopic.length() > 0) {
				if (mqttClient.subscribe(responseTopic.c_str(), 1)) {  // QoS=1确保订阅成功
					Serial.println("[MQTT] Subscribed to: " + responseTopic);
				}
				else {
					Serial.println("[MQTT] Failed to subscribe: " + responseTopic);
				}
			}

			// 重置失败计数
			consecutiveMQTTFailures = 0;
			return true;
		}
		else {
			Serial.printf("[MQTT] Fail, state=%d. Retry in %lu ms\n",
				mqttClient.state(), retryDelay);
			delay(retryDelay);

			// 指数退避，最大延迟10秒
			retryDelay = min(retryDelay * 2, 10000UL);
		}
	}

	return false;
}

/**
 * @brief 保持 MQTT 在线（重连 + loop + WiFi健康检查 + 自动重启）
 */
void maintainMQTT(unsigned long timeoutMs) {
	unsigned long now = millis();

	// 定期检查WiFi健康状态（每30秒）
	if (now - lastWiFiCheckTime > 30000) {
		lastWiFiCheckTime = now;

		if (WiFi.status() != WL_CONNECTED) {
			Serial.printf("[WiFi] Lost connection (status=%d), attempting reconnect...\n", WiFi.status());
			connectToWiFi(15000);
		}
		else {
			// 检查信号强度，过弱时可能需要重连
			int rssi = WiFi.RSSI();
			if (rssi < -85 && rssi != 0) {
				Serial.printf("[WiFi] Weak signal (%d dBm), consider reconnecting\n", rssi);
			}
		}

		// 定期测试网络可用性（每30秒）
		if (WiFi.status() == WL_CONNECTED) {
			bool networkAvailable = checkNetworkAvailable();
			Serial.printf("[Network] Availability check: %s\n", networkAvailable ? "OK" : "FAIL");
		}
	}

	// 检查并触发自动重启（在WiFi健康检查后）
	checkAutoRestart();

	// 检查MQTT连接状态
	if (!mqttClient.connected()) {
		Serial.printf("[MQTT] Disconnected (state=%d), reconnecting...\n", mqttClient.state());
		connectToMQTT(timeoutMs);
	}

	// 定期调用loop()处理MQTT心跳
	mqttClient.loop();

	// 检查上次发布成功时间，如果超过阈值可能需要重连
	if (lastMQTTPublishSuccess > 0 && (now - lastMQTTPublishSuccess > MQTT_KEEPALIVE_INTERVAL * 2)) {
		Serial.printf("[MQTT] No successful publish for %lu seconds, forcing reconnect...\n",
			(now - lastMQTTPublishSuccess) / 1000);
		mqttClient.disconnect();
		connectToMQTT(timeoutMs);
	}
}

/**
 * @brief 通过 MQTT 发布数据（带智能重连和超时控制）
 */
bool publishData(const String& topic, const String& payload, unsigned long timeoutMs) {
	unsigned long start = millis();
	unsigned long retryDelay = 300;  // 初始重试延迟
	int retryCount = 0;

	// 确保MQTT已连接
	while (!mqttClient.connected()) {
		if (millis() - start > timeoutMs) {
			Serial.printf("[MQTT] publishData: connect timeout >%lu ms\n", timeoutMs);
			return false;
		}

		unsigned long remainingTime = timeoutMs - (millis() - start);
		if (remainingTime > 10000) {  // 给重连留足够时间
			connectToMQTT(10000);
		} else {
			connectToMQTT(remainingTime);
		}

		if (!mqttClient.connected()) {
			Serial.printf("[MQTT] publishData: reconnect attempt %d failed, wait %lu ms\n",
				retryCount + 1, retryDelay);
			delay(retryDelay);
			retryDelay = min(retryDelay * 2, 3000UL);  // 指数退避，最大3秒
			retryCount++;
		}
	}

	// MQTT已连接，尝试发布
	while (millis() - start < timeoutMs) {
		if (mqttClient.publish(topic.c_str(), payload.c_str(), false)) {  // QoS=0, retain=false
			Serial.println("[MQTT] Publish success");
			// Serial.println(payload);  // 完整消息可能太长，注释掉减少日志
			lastMQTTPublishSuccess = millis();
			consecutiveMQTTFailures = 0;
			return true;
		}
		else {
			Serial.printf("[MQTT] Publish fail (attempt %d), state=%d. Retry in %lu ms\n",
				++retryCount, mqttClient.state(), retryDelay);
			delay(retryDelay);
			retryDelay = min(retryDelay * 2, 2000UL);  // 指数退避，最大2秒
			consecutiveMQTTFailures++;

			// 连续失败次数过多，强制重连
			if (consecutiveMQTTFailures >= MAX_CONSECUTIVE_FAILURES) {
				Serial.printf("[MQTT] Too many consecutive failures (%d), forcing reconnect\n",
					MAX_CONSECUTIVE_FAILURES);
				mqttClient.disconnect();
				connectToMQTT(timeoutMs - (millis() - start));
				retryDelay = 300;  // 重置延迟
				continue;
			}

			// 检查连接状态
			if (!mqttClient.connected()) {
				Serial.println("[MQTT] Connection lost during publish, reconnecting...");
				unsigned long remainingTime = timeoutMs - (millis() - start);
				if (remainingTime > 5000) {
					connectToMQTT(5000);
				}
			}
		}
	}

	Serial.printf("[MQTT] publishData: overall timeout >%lu ms after %d attempts\n",
		timeoutMs, retryCount);
	return false;
}
