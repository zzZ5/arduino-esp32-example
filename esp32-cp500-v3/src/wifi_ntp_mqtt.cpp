#include "wifi_ntp_mqtt.h"
#include "config_manager.h"
#include <WiFi.h>
#include <time.h>
#include <PubSubClient.h>
#include <ESP.h>

extern String getTelemetryTopic();
extern String getResponseTopic();

static WiFiClient espClient;
PubSubClient mqttClient(espClient);

static unsigned long lastWiFiCheckTime = 0;
static unsigned long lastMQTTPublishSuccess = 0;
static unsigned long consecutiveWiFiFailures = 0;
static unsigned long consecutiveMQTTFailures = 0;

static const unsigned long MQTT_KEEPALIVE_INTERVAL = 60000;
static const unsigned long MAX_CONSECUTIVE_FAILURES = 10;

PubSubClient& getMQTTClient() {
	return mqttClient;
}

static void reportOfflineModeIfNeeded() {
	if (consecutiveWiFiFailures > 0 && (consecutiveWiFiFailures % 5) == 0) {
		Serial.printf(
			"[WiFi] Connection failed %lu times, keeping local control active and continuing background reconnect attempts\n",
			consecutiveWiFiFailures);
	}
}

bool connectToWiFi(unsigned long timeoutMs) {
	if (WiFi.status() == WL_CONNECTED && WiFi.gatewayIP().toString() != "0.0.0.0") {
		return true;
	}

	WiFi.disconnect();
	delay(100);
	WiFi.mode(WIFI_STA);
	WiFi.setAutoReconnect(true);
	WiFi.setSleep(false);
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
			Serial.printf("[WiFi] Consecutive failures: %lu\n", consecutiveWiFiFailures);
			reportOfflineModeIfNeeded();
			return false;
		}
	}

	Serial.printf("\n[WiFi] Connected, IP: %s, RSSI: %d dBm\n",
		WiFi.localIP().toString().c_str(), WiFi.RSSI());

	consecutiveWiFiFailures = 0;
	return true;
}

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

bool multiNTPSetup(unsigned long totalTimeoutMs) {
	unsigned long start = millis();
	bool synced = false;
	String syncedServer = "";

	while (!synced) {
		for (const auto& server : appConfig.ntpServers) {
			if (millis() - start > totalTimeoutMs) {
				Serial.println("[NTP] Overall timeout!");
				return false;
			}

			if (server.length() < 1) continue;

			Serial.print("[NTP] Trying server: ");
			Serial.println(server);

			configTime(0, 0, server.c_str());
			if (waitForSync(3000)) {
				Serial.println("[NTP] Sync success");
				syncedServer = server;
				synced = true;
				break;
			}

			Serial.println("[NTP] Failed, trying next server...");
		}

		if (!synced) {
			if (millis() - start > totalTimeoutMs) {
				Serial.println("[NTP] Overall timeout (retry loop)");
				return false;
			}
			Serial.println("[NTP] All servers failed, retry after 2 seconds...");
			delay(2000);
		}
	}

	if (syncedServer.length() == 0) {
		Serial.println("[NTP] No synced server recorded!");
		return false;
	}

	configTime(8 * 3600, 0, syncedServer.c_str());

	struct tm tinfo;
	if (getLocalTime(&tinfo)) {
		Serial.println("[NTP] Timezone set to UTC+8, time validated");
		Serial.printf("[NTP] Current time: %04d-%02d-%02d %02d:%02d:%02d\n",
			tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday,
			tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec);
		return true;
	}

	Serial.println("[NTP] Timezone set but validation failed");
	return false;
}

String getTimeString() {
	struct tm tinfo;
	if (!getLocalTime(&tinfo)) {
		return "1970-01-01 00:00:00";
	}
	char buf[20];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tinfo);
	return String(buf);
}

String getPublicIP() {
	String localIP = WiFi.localIP().toString();
	Serial.printf("[PublicIP] Using local IP for boot payload: %s\n", localIP.c_str());
	return localIP;
}

bool connectToMQTT(unsigned long timeoutMs) {
	mqttClient.setServer(appConfig.mqttServer.c_str(), appConfig.mqttPort);
	mqttClient.setBufferSize(4096);

	unsigned long start = millis();
	unsigned long retryDelay = 500;
	int retryCount = 0;

	while (!mqttClient.connected()) {
		if (WiFi.status() != WL_CONNECTED) {
			Serial.println("[MQTT] WiFi not connected, reconnecting...");
			if (!connectToWiFi(15000)) {
				Serial.println("[MQTT] WiFi reconnect failed, waiting before retry...");
				delay(5000);
				continue;
			}
		}

		if (millis() - start > timeoutMs) {
			Serial.printf("[MQTT] Connect timeout (> %lu ms)\n", timeoutMs);
			return false;
		}

		String clientId = "cp500_" + appConfig.mqttDeviceCode + "_" + String(millis() % 10000);
		Serial.printf("[MQTT] Connecting to %s:%d as %s... (attempt %d)\n",
			appConfig.mqttServer.c_str(), appConfig.mqttPort, clientId.c_str(), ++retryCount);

		if (mqttClient.connect(clientId.c_str(),
			appConfig.mqttUser.c_str(),
			appConfig.mqttPass.c_str())) {
			Serial.println("[MQTT] Connected");

			String responseTopic = getResponseTopic();
			if (responseTopic.length() > 0) {
				if (mqttClient.subscribe(responseTopic.c_str(), 1)) {
					Serial.println("[MQTT] Subscribed to: " + responseTopic);
				} else {
					Serial.println("[MQTT] Failed to subscribe: " + responseTopic);
				}
			}

			consecutiveMQTTFailures = 0;
			return true;
		}

		Serial.printf("[MQTT] Connect failed, state=%d. Retry in %lu ms\n",
			mqttClient.state(), retryDelay);
		delay(retryDelay);
		retryDelay = min(retryDelay * 2, 10000UL);
	}

	return false;
}

void maintainMQTT(unsigned long timeoutMs) {
	if (millis() - lastWiFiCheckTime > 30000) {
		lastWiFiCheckTime = millis();

		if (WiFi.status() != WL_CONNECTED) {
			Serial.printf("[WiFi] Lost connection (status=%d), attempting reconnect...\n", WiFi.status());
			connectToWiFi(15000);
		} else {
			int rssi = WiFi.RSSI();
			if (rssi < -85 && rssi != 0) {
				Serial.printf("[WiFi] Weak signal (%d dBm), consider reconnecting\n", rssi);
			}
		}
	}

	if (!mqttClient.connected()) {
		Serial.printf("[MQTT] Disconnected (state=%d), reconnecting...\n", mqttClient.state());
		connectToMQTT(timeoutMs);
	}

	mqttClient.loop();

	if (lastMQTTPublishSuccess > 0 &&
		(millis() - lastMQTTPublishSuccess > MQTT_KEEPALIVE_INTERVAL * 2)) {
		Serial.printf("[MQTT] No successful publish for %lu seconds, forcing reconnect...\n",
			(millis() - lastMQTTPublishSuccess) / 1000);
		mqttClient.disconnect();
		connectToMQTT(timeoutMs);
	}
}

bool publishData(const String& topic, const String& payload, unsigned long timeoutMs) {
	unsigned long start = millis();
	unsigned long retryDelay = 300;
	int retryCount = 0;

	while (!mqttClient.connected()) {
		if (millis() - start > timeoutMs) {
			Serial.printf("[MQTT] publishData: connect timeout >%lu ms\n", timeoutMs);
			return false;
		}

		unsigned long remainingTime = timeoutMs - (millis() - start);
		if (remainingTime > 10000) {
			connectToMQTT(10000);
		} else {
			connectToMQTT(remainingTime);
		}

		if (!mqttClient.connected()) {
			Serial.printf("[MQTT] publishData: reconnect attempt %d failed, wait %lu ms\n",
				retryCount + 1, retryDelay);
			delay(retryDelay);
			retryDelay = min(retryDelay * 2, 3000UL);
			retryCount++;
		}
	}

	while (millis() - start < timeoutMs) {
		if (mqttClient.publish(topic.c_str(), payload.c_str(), false)) {
			Serial.println("[MQTT] Publish success");
			lastMQTTPublishSuccess = millis();
			consecutiveMQTTFailures = 0;
			return true;
		}

		Serial.printf("[MQTT] Publish fail (attempt %d), state=%d. Retry in %lu ms\n",
			++retryCount, mqttClient.state(), retryDelay);
		delay(retryDelay);
		retryDelay = min(retryDelay * 2, 2000UL);
		consecutiveMQTTFailures++;

		if (consecutiveMQTTFailures >= MAX_CONSECUTIVE_FAILURES) {
			Serial.printf("[MQTT] Too many consecutive failures (%d), forcing reconnect\n",
				MAX_CONSECUTIVE_FAILURES);
			mqttClient.disconnect();
			connectToMQTT(timeoutMs - (millis() - start));
			retryDelay = 300;
			continue;
		}

		if (!mqttClient.connected()) {
			Serial.println("[MQTT] Connection lost during publish, reconnecting...");
			unsigned long remainingTime = timeoutMs - (millis() - start);
			if (remainingTime > 5000) {
				connectToMQTT(5000);
			}
		}
	}

	Serial.printf("[MQTT] publishData: overall timeout >%lu ms after %d attempts\n",
		timeoutMs, retryCount);
	return false;
}
