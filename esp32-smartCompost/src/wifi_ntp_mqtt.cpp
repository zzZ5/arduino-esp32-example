// wifi_ntp_mqtt.cpp
#include "wifi_ntp_mqtt.h"
#include "config_manager.h"
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>

// Arduino loopTask is subscribed to the Task WDT (~5s). Long WiFi/MQTT waits
// must reset it or the chip reboots (often mistaken for "random" restarts).
static inline void feedTaskWatchdog() {
	esp_task_wdt_reset();
}

// Global WiFi client and MQTT client.
static WiFiClient espClient;
PubSubClient mqttClient(espClient);

// PubSubClient is not thread-safe. measurementTask (Core 1) publishes while
// loop() runs mqttClient.loop() on Core 0; serialize all MQTT API usage.
static SemaphoreHandle_t g_mqttMutex = nullptr;

static void ensureMqttMutex() {
	if (!g_mqttMutex) {
		g_mqttMutex = xSemaphoreCreateMutex();
	}
}

// Periodic WiFi health check state.
static unsigned long lastWiFiCheck = 0;
static const unsigned long WIFI_CHECK_INTERVAL = 30000;

// WiFi reconnect failure counter.
static int wifiFailCount = 0;
static const int WIFI_FAIL_LIMIT = 5;

// Expose the shared MQTT client instance.
PubSubClient& getMQTTClient() {
	return mqttClient;
}

// Keep WiFi connected and retry in place when the link is lost.
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
		feedTaskWatchdog();
		if (!connectToWiFi(10000)) {
			Serial.println("[WiFi] Reconnect failed");
			wifiFailCount++;
			Serial.printf("[WiFi] Fail count: %d/%d\n", wifiFailCount, WIFI_FAIL_LIMIT);

			if (wifiFailCount >= WIFI_FAIL_LIMIT) {
				Serial.println("[WiFi] Too many failures, stay offline and keep retrying");
				wifiFailCount = WIFI_FAIL_LIMIT;
			}
		}
		else {
			wifiFailCount = 0;
		}
	}
}

bool connectToWiFi(unsigned long timeoutMs) {
	WiFi.mode(WIFI_STA);
	WiFi.begin(appConfig.wifiSSID.c_str(), appConfig.wifiPass.c_str());

	Serial.print("[WiFi] Connecting to: ");
	Serial.println(appConfig.wifiSSID);

	unsigned long start = millis();
	while (WiFi.status() != WL_CONNECTED) {
		delay(500);
		feedTaskWatchdog();
		if (millis() - start > timeoutMs) {
			Serial.println("\n[WiFi] Timeout!");
			return false;
		}
	}

	Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
	return true;
}

// Wait for NTP time to become valid for up to waitMs milliseconds.
static bool waitForSync(unsigned long waitMs) {
	unsigned long start = millis();
	struct tm tinfo;
	while ((millis() - start) < waitMs) {
		if (getLocalTime(&tinfo)) {
			return true;
		}
		delay(100);
		feedTaskWatchdog();
	}
	return false;
}

// Try multiple NTP servers until time sync succeeds or the overall timeout expires.
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
			feedTaskWatchdog();
		}
	}

	// Use UTC+8 after successful sync.
	configTime(8 * 3600, 0, appConfig.ntpServers[0].c_str());
	Serial.println("[NTP] Timezone set to UTC+8");
	return true;
}

// Return the current local time string, or a fixed fallback when time is invalid.
String getTimeString() {
	struct tm tinfo;
	if (!getLocalTime(&tinfo)) {
		return "1970-01-01 00:00:00";
	}

	if (tinfo.tm_year < 120) {
		return "1970-01-01 00:00:00";
	}

	char buf[20];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tinfo);
	return String(buf);
}

bool connectToMQTT(unsigned long timeoutMs) {
	mqttClient.setServer(appConfig.mqttServer.c_str(), appConfig.mqttPort);
	mqttClient.setBufferSize(1024);
	// Cap blocking TCP time per attempt so loopTask can satisfy the TWDT.
	espClient.setTimeout(4000);

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

		feedTaskWatchdog();
		Serial.printf("[MQTT] Connecting to %s:%d...\n", appConfig.mqttServer.c_str(), appConfig.mqttPort);
		if (mqttClient.connect(appConfig.mqttClientId.c_str(),
			appConfig.mqttUser.c_str(),
			appConfig.mqttPass.c_str())) {
			Serial.println("[MQTT] Connected.");

			// Resubscribe after reconnect because PubSubClient does not keep subscriptions.
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
			feedTaskWatchdog();
		}
	}

	return false;
}

// Try several public services and fall back to the local LAN IP when needed.
String getPublicIP() {
	String localIP = WiFi.localIP().toString();
	Serial.printf("[IP] Local IP: %s\n", localIP.c_str());

	const char* ipServices[] = {
		"http://ifconfig.me/ip",
		"http://icanhazip.com",
		"http://ipecho.net/plain",
		"http://api.ipify.org"
	};

	HTTPClient http;
	http.setTimeout(5000);

	for (const char* url : ipServices) {
		if (WiFi.status() != WL_CONNECTED) {
			break;
		}

		Serial.printf("[IP] Trying: %s\n", url);
		if (http.begin(url)) {
			int httpCode = http.GET();
			if (httpCode == HTTP_CODE_OK) {
				String publicIP = http.getString();
				publicIP.trim();
				http.end();

				if (publicIP.length() > 0) {
					Serial.printf("[IP] Public IP: %s\n", publicIP.c_str());
					return publicIP;
				}
			}
			http.end();
		}
		delay(500);
	}

	Serial.println("[IP] Failed to get public IP, using local IP");
	return localIP;
}

// Keep WiFi and MQTT alive during normal runtime.
void maintainMQTT(unsigned long timeoutMs) {
	maintainWiFi();

	ensureMqttMutex();
	// Do not block loop() on the mutex: publishData may hold it for many seconds.
	if (xSemaphoreTake(g_mqttMutex, pdMS_TO_TICKS(120)) != pdTRUE) {
		return;
	}

	if (!mqttClient.connected()) {
		Serial.println("[MQTT] Not connected, reconnecting...");
		// One short reconnect attempt per loop() — avoids TWDT when broker is down.
		unsigned long slice = timeoutMs;
		if (slice > 4000UL) {
			slice = 4000UL;
		}
		connectToMQTT(slice);
	}
	if (mqttClient.connected()) {
		mqttClient.loop();
	}
	xSemaphoreGive(g_mqttMutex);
}

bool publishData(const String& topic, const String& payload, unsigned long timeoutMs) {
	ensureMqttMutex();
	maintainWiFi();

	if (xSemaphoreTake(g_mqttMutex, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
		Serial.printf("[MQTT] publishData: mutex timeout >%lu ms\n", timeoutMs);
		return false;
	}

	bool ok = false;
	unsigned long start = millis();

	while (!mqttClient.connected()) {
		if (millis() - start > timeoutMs) {
			Serial.printf("[MQTT] publishData: connect timeout >%lu ms\n", timeoutMs);
			xSemaphoreGive(g_mqttMutex);
			return false;
		}
		maintainWiFi();
		connectToMQTT(timeoutMs - (millis() - start));
	}

	while (millis() - start < timeoutMs) {
		mqttClient.loop();
		if (mqttClient.publish(topic.c_str(), payload.c_str())) {
			Serial.println("[MQTT] Publish success:");
			Serial.println(payload);
			ok = true;
			break;
		}
		Serial.printf("[MQTT] Publish fail, state=%d. Retry in 300ms\n", mqttClient.state());
		delay(300);
		feedTaskWatchdog();
		maintainWiFi();
		if (!mqttClient.connected()) {
			connectToMQTT(timeoutMs - (millis() - start));
		}
	}

	if (!ok) {
		Serial.printf("[MQTT] publishData: overall timeout >%lu ms\n", timeoutMs);
	}

	xSemaphoreGive(g_mqttMutex);
	return ok;
}

// Publish immediately when possible, otherwise persist data locally for later upload.
bool publishDataOrCache(const String& topic, const String& payload, const String& timestamp, unsigned long timeoutMs) {
	if (publishData(topic, payload, timeoutMs)) {
		return true;
	}

	Serial.println("[MQTT] Publish failed, caching locally...");
	extern bool savePendingData(const String&, const String&, const String&);

	if (savePendingData(topic, payload, timestamp)) {
		Serial.println("[MQTT] Data cached successfully");
		return false;
	}
	else {
		Serial.println("[MQTT] Failed to cache data");
		return false;
	}
}

// Attempt to upload pending cached samples in small batches.
int uploadCachedData(int maxUpload) {
	extern int getPendingDataCount();
	extern bool getFirstPendingData(String&, String&, String&);
	extern bool markFirstDataAsUploaded();
	extern bool deferFirstPendingDataAfterFailure();
	extern int cleanUploadedData(int keepCount);

	int pendingCount = getPendingDataCount();
	if (pendingCount <= 0) {
		return 0;
	}

	Serial.printf("[Cache] Found %d pending data items, uploading up to %d...\n", pendingCount, maxUpload);

	const int maxFailuresPerRun = 2;
	int uploadedCount = 0;
	int failureCount = 0;
	for (int i = 0; i < maxUpload; i++) {
		String topic, payload, timestamp;

		if (!getFirstPendingData(topic, payload, timestamp)) {
			Serial.println("[Cache] No more pending data");
			break;
		}

		Serial.printf("[Cache] Uploading cached data (timestamp: %s)...\n", timestamp.c_str());

		if (publishData(topic, payload, 5000)) {
			Serial.println("[Cache] Cached data uploaded successfully");
			markFirstDataAsUploaded();
			uploadedCount++;
			failureCount = 0;
			delay(200);
		}
		else {
			Serial.println("[Cache] Upload failed, keeping cached data for next retry");
			deferFirstPendingDataAfterFailure();
			failureCount++;
			if (failureCount >= maxFailuresPerRun) {
				Serial.println("[Cache] Too many failures in this run, stop uploading");
				break;
			}
			delay(200);
		}
	}

	if (uploadedCount > 0) {
		Serial.printf("[Cache] Uploaded %d cached data items\n", uploadedCount);

		// Keep the latest uploaded record for basic auditability.
		int cleaned = cleanUploadedData(1);
		if (cleaned > 0) {
			Serial.printf("[Cache] Cleaned %d old uploaded items (kept latest 1)\n", cleaned);
		}
	}
	else {
		Serial.println("[Cache] No cached data uploaded");
	}

	return uploadedCount;
}
