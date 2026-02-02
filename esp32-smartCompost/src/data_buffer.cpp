// data_buffer.cpp
// 断网数据缓存模块实现
// 使用单个JSON文件存储所有缓存数据

#include "data_buffer.h"
#include <SPIFFS.h>
#include "config_manager.h"
#include <vector>
#include <limits.h>
#include "wifi_ntp_mqtt.h"

// ========== 全局变量 ==========

static int g_maxCacheCount = 100;
static int g_maxCacheDays = 7;
static const char* CACHE_FILE = "/data_cache.json";
static const char* CACHE_TEMP_FILE = "/data_cache.tmp";
static const char* CACHE_BACKUP_FILE = "/data_cache.bak";

// ========== 内部结构 ==========

struct CacheItem {
    String topic;
    String payload;
    String timestamp;
    unsigned long epoch;
    bool uploaded;
    uint8_t retryCount;
};

// ========== 内部工具函数 ==========

/**
 * @brief 获取缓存文件路径
 */
const char* getCacheFilePath() {
    return CACHE_FILE;
}

/**
 * @brief 检查缓存是否超过限制数量
 */
static bool isCacheFull(int currentCount) {
    return (currentCount >= g_maxCacheCount);
}

/**
 * @brief 从时间字符串解析为epoch
 */
static time_t parseTimestampToEpoch(const String& ts) {
    // 格式: "YYYY-MM-DD HH:MM:SS"
    struct tm tm {};
    if (strptime(ts.c_str(), "%Y-%m-%d %H:%M:%S", &tm)) {
        return mktime(&tm);
    }
    return 0;
}

/**
 * @brief 获取当前缓存用 epoch（未同步时回退到 millis）
 */
static unsigned long getCacheEpoch() {
    time_t nowEpoch = time(nullptr);
    return (nowEpoch > 0) ? (unsigned long)nowEpoch : millis();
}

/**
 * @brief 选择并删除一条最旧的缓存数据（优先已上传）
 */
static bool evictOldestItem(std::vector<CacheItem>& items) {
    if (items.empty()) {
        return false;
    }

    int oldestUploadedIndex = -1;
    unsigned long oldestUploadedEpoch = ULONG_MAX;
    for (int i = 0; i < (int)items.size(); i++) {
        if (!items[i].uploaded) continue;
        if (items[i].epoch < oldestUploadedEpoch) {
            oldestUploadedEpoch = items[i].epoch;
            oldestUploadedIndex = i;
        }
    }

    if (oldestUploadedIndex >= 0) {
        items.erase(items.begin() + oldestUploadedIndex);
        Serial.println("[Cache] Evicted oldest uploaded item");
        return true;
    }

    int oldestIndex = 0;
    unsigned long oldestEpoch = items[0].epoch;
    for (int i = 1; i < (int)items.size(); i++) {
        if (items[i].epoch < oldestEpoch) {
            oldestEpoch = items[i].epoch;
            oldestIndex = i;
        }
    }
    items.erase(items.begin() + oldestIndex);
    Serial.println("[Cache] Evicted oldest pending item (cache full)");
    return true;
}

/**
 * @brief 加载缓存文件到内存
 */
static bool loadCacheFromFile(std::vector<CacheItem>& items) {
    if (!SPIFFS.exists(CACHE_FILE)) {
        return true;  // 文件不存在，视为空缓存
    }

    File file = SPIFFS.open(CACHE_FILE, FILE_READ);
    if (!file) {
        Serial.println("[Cache] Failed to open cache file for reading");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        Serial.printf("[Cache] JSON parse error: %s\n", err.c_str());
        if (SPIFFS.exists(CACHE_FILE)) {
            String badPath = String(CACHE_FILE) + ".corrupt";
            if (SPIFFS.exists(badPath)) {
                SPIFFS.remove(badPath);
            }
            if (!SPIFFS.rename(CACHE_FILE, badPath)) {
                Serial.println("[Cache] Failed to backup corrupt cache file");
            }
            else {
                Serial.println("[Cache] Corrupt cache file moved aside");
            }
        }
        items.clear();
        return true;
    }

    // 解析数组
    if (!doc["data"].is<JsonArray>()) {
        Serial.println("[Cache] Invalid cache file format");
        return false;
    }

    JsonArray arr = doc["data"].as<JsonArray>();
    items.clear();
    items.reserve(arr.size());

    for (JsonObject item : arr) {
        CacheItem cacheItem;
        cacheItem.topic = item["topic"].as<String>();
        cacheItem.payload = item["payload"].as<String>();
        cacheItem.timestamp = item["timestamp"].as<String>();
        cacheItem.epoch = item["epoch"].as<unsigned long>();
        cacheItem.uploaded = item["uploaded"] | false;
        cacheItem.retryCount = item["retryCount"] | 0;
        items.push_back(cacheItem);
    }

    Serial.printf("[Cache] Loaded %d items from file\n", items.size());
    return true;
}

/**
 * @brief 保存内存中的缓存到文件
 */
static bool saveCacheToFile(const std::vector<CacheItem>& items) {
    File file = SPIFFS.open(CACHE_TEMP_FILE, FILE_WRITE);
    if (!file) {
        Serial.println("[Cache] Failed to open cache file for writing");
        return false;
    }

    JsonDocument doc;
    JsonArray arr = doc["data"].to<JsonArray>();

    for (const auto& item : items) {
        JsonObject obj = arr.add<JsonObject>();
        obj["topic"] = item.topic;
        obj["payload"] = item.payload;
        obj["timestamp"] = item.timestamp;
        obj["epoch"] = item.epoch;
        obj["uploaded"] = item.uploaded;
        obj["retryCount"] = item.retryCount;
    }

    if (serializeJson(doc, file) == 0) {
        Serial.println("[Cache] Failed to write cache file");
        file.close();
        return false;
    }

    file.close();
    if (SPIFFS.exists(CACHE_BACKUP_FILE)) {
        SPIFFS.remove(CACHE_BACKUP_FILE);
    }
    if (SPIFFS.exists(CACHE_FILE)) {
        if (!SPIFFS.rename(CACHE_FILE, CACHE_BACKUP_FILE)) {
            Serial.println("[Cache] Failed to backup old cache file");
        }
    }
    if (!SPIFFS.rename(CACHE_TEMP_FILE, CACHE_FILE)) {
        Serial.println("[Cache] Failed to replace cache file");
        if (SPIFFS.exists(CACHE_BACKUP_FILE)) {
            SPIFFS.rename(CACHE_BACKUP_FILE, CACHE_FILE);
        }
        return false;
    }
    if (SPIFFS.exists(CACHE_BACKUP_FILE)) {
        SPIFFS.remove(CACHE_BACKUP_FILE);
    }
    return true;
}

// ========== 初始化与配置 ==========

bool initDataBuffer(int maxCacheCount, int maxCacheDays) {
    g_maxCacheCount = maxCacheCount;
    g_maxCacheDays = maxCacheDays;

    Serial.printf("[Cache] Init: maxCount=%d, maxDays=%d\n", g_maxCacheCount, g_maxCacheDays);

    // 清理过期数据
    cleanExpiredCache();

    return true;
}

void cleanExpiredCache() {
    Serial.println("[Cache] Checking expired data...");

    std::vector<CacheItem> items;
    if (!loadCacheFromFile(items)) {
        return;
    }

    if (items.empty()) {
        Serial.println("[Cache] No data to clean");
        return;
    }

    time_t now = time(nullptr);
    if (now <= 0) {
        Serial.println("[Cache] Time not synced, skip expiration check");
        return;
    }

    unsigned long maxAgeSeconds = g_maxCacheDays * 24 * 3600UL;
    int originalCount = items.size();

    // 移除过期数据
    items.erase(
        std::remove_if(items.begin(), items.end(),
            [now, maxAgeSeconds](const CacheItem& item) {
                return (item.epoch > 0 && (now - (time_t)item.epoch) > (long)maxAgeSeconds);
            }),
        items.end()
    );

    int deletedCount = originalCount - items.size();
    if (deletedCount > 0) {
        Serial.printf("[Cache] Cleaned %d expired items\n", deletedCount);
        saveCacheToFile(items);
    }
    else {
        Serial.println("[Cache] No expired data found");
    }
}

// ========== 数据存储 ==========

bool savePendingData(const String& topic, const String& payload, const String& timestamp) {
    std::vector<CacheItem> items;
    if (!loadCacheFromFile(items)) {
        return false;
    }

    // 检查缓存是否已满
    if (isCacheFull((int)items.size())) {
        Serial.println("[Cache] Cache full, trying to evict oldest item");
        if (!evictOldestItem(items)) {
            Serial.println("[Cache] Cache full, cannot evict item");
            return false;
        }
    }

    // 生成时间戳
    unsigned long fileTime = getCacheEpoch();
    String ts = (timestamp.length() > 0) ? timestamp : getTimeString();

    // 添加新数据
    CacheItem newItem;
    newItem.topic = topic;
    newItem.payload = payload;
    newItem.timestamp = ts;
    newItem.epoch = fileTime;
    newItem.uploaded = false;
    newItem.retryCount = 0;

    items.push_back(newItem);

    // 保存到文件
    if (saveCacheToFile(items)) {
        Serial.printf("[Cache] Saved new data (topic: %s, total: %d)\n", topic.c_str(), items.size());
        return true;
    }

    return false;
}

// ========== 数据读取与标记 ==========

int getPendingDataCount() {
    std::vector<CacheItem> items;
    if (!loadCacheFromFile(items)) {
        return -1;
    }

    // 统计未上传的数据
    int count = 0;
    for (const auto& item : items) {
        if (!item.uploaded) {
            count++;
        }
    }
    return count;
}

bool getFirstPendingData(String& outTopic, String& outPayload, String& outTimestamp) {
    std::vector<CacheItem> items;
    if (!loadCacheFromFile(items)) {
        return false;
    }

    // 按epoch排序（最旧的在前）
    std::sort(items.begin(), items.end(),
        [](const CacheItem& a, const CacheItem& b) {
            return a.epoch < b.epoch;
        });

    // 查找第一个未上传的数据
    for (const auto& item : items) {
        if (!item.uploaded) {
            outTopic = item.topic;
            outPayload = item.payload;
            outTimestamp = item.timestamp;
            return true;
        }
    }

    return false;
}

bool markFirstDataAsUploaded() {
    std::vector<CacheItem> items;
    if (!loadCacheFromFile(items)) {
        return false;
    }

    // 按epoch排序（最旧的在前）
    std::sort(items.begin(), items.end(),
        [](const CacheItem& a, const CacheItem& b) {
            return a.epoch < b.epoch;
        });

    // 查找第一个未上传的数据并标记
    for (auto& item : items) {
        if (!item.uploaded) {
            item.uploaded = true;
            Serial.printf("[Cache] Marked data as uploaded (timestamp: %s)\n", item.timestamp.c_str());
            return saveCacheToFile(items);
        }
    }

    Serial.println("[Cache] No pending data to mark");
    return false;
}

bool deferFirstPendingDataAfterFailure() {
    std::vector<CacheItem> items;
    if (!loadCacheFromFile(items)) {
        return false;
    }

    // 按epoch排序（最旧的在前）
    std::sort(items.begin(), items.end(),
        [](const CacheItem& a, const CacheItem& b) {
            return a.epoch < b.epoch;
        });

    // 查找第一个未上传的数据并延后
    for (auto& item : items) {
        if (!item.uploaded) {
            if (item.retryCount < 255) {
                item.retryCount++;
            }
            item.epoch = getCacheEpoch();  // 延后到当前时间，避免卡住队列
            Serial.printf("[Cache] Deferred data after failure (retry=%u)\n", item.retryCount);
            return saveCacheToFile(items);
        }
    }

    Serial.println("[Cache] No pending data to defer");
    return false;
}

int cleanUploadedData(int keepCount) {
    std::vector<CacheItem> items;
    if (!loadCacheFromFile(items)) {
        return 0;
    }

    if (items.empty()) {
        return 0;
    }

    // 统计已上传数据
    std::vector<int> uploadedIndices;
    for (int i = 0; i < (int)items.size(); i++) {
        if (items[i].uploaded) {
            uploadedIndices.push_back(i);
        }
    }

    if ((int)uploadedIndices.size() <= keepCount) {
        return 0;  // 没有需要清理的
    }

    // 按epoch排序已上传数据（最新的在后面）
    std::sort(uploadedIndices.begin(), uploadedIndices.end(),
        [&items](int a, int b) {
            return items[a].epoch < items[b].epoch;
        });

    // 删除最旧的已上传数据，保留最近keepCount条
    int toDelete = uploadedIndices.size() - keepCount;

    // 注意：从后往前删除，避免索引偏移
    for (int i = toDelete - 1; i >= 0; i--) {
        items.erase(items.begin() + uploadedIndices[i]);
    }

    int deletedCount = toDelete;
    if (deletedCount > 0) {
        Serial.printf("[Cache] Cleaned %d old uploaded items (kept %d)\n", deletedCount, keepCount);
        saveCacheToFile(items);
    }

    return deletedCount;
}

bool clearAllPendingData() {
    if (SPIFFS.remove(CACHE_FILE)) {
        Serial.println("[Cache] Cleared all cache");
        return true;
    }
    return false;
}
