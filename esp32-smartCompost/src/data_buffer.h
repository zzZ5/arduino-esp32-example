// data_buffer.h
// 断网数据缓存模块
// 功能：断网时将数据暂存到SPIFFS，网络恢复后自动上传
// 使用单个JSON文件存储所有缓存数据

#ifndef DATA_BUFFER_H
#define DATA_BUFFER_H

#include <Arduino.h>
#include <ArduinoJson.h>

// ========== 初始化与配置 ==========

/**
 * @brief 初始化数据缓存模块
 * @param maxCacheCount 最大缓存条目数（默认100）
 * @param maxCacheDays 最大缓存天数，超过自动清理（默认7天）
 * @return true 成功 false 失败
 */
bool initDataBuffer(int maxCacheCount = 100, int maxCacheDays = 7);

/**
 * @brief 清理过期缓存
 */
void cleanExpiredCache();

// ========== 数据存储 ==========

/**
 * @brief 保存一条待上传数据（MQTT payload）
 * @param topic MQTT主题
 * @param payload 数据payload（JSON字符串）
 * @param timestamp 时间戳字符串（可为空，将使用当前时间）
 * @return true 成功 false 失败
 */
bool savePendingData(const String& topic, const String& payload, const String& timestamp = "");

// ========== 数据读取与标记 ==========

/**
 * @brief 获取缓存中的数据条目数
 * @return 数据条目数，-1表示出错
 */
int getPendingDataCount();

/**
 * @brief 获取第一条待上传数据（未上传的最旧数据）
 * @param outTopic 输出主题
 * @param outPayload 输出payload
 * @param outTimestamp 输出时间戳
 * @return true 成功读取 false 队列为空或失败
 */
bool getFirstPendingData(String& outTopic, String& outPayload, String& outTimestamp);

/**
 * @brief 标记当前第一条数据为已上传
 * @return true 成功 false 失败
 */
bool markFirstDataAsUploaded();

/**
 * @brief 清理已上传的数据（只保留最近N条）
 * @param keepCount 保留条数（默认1条）
 * @return 清理的条数
 */
int cleanUploadedData(int keepCount = 1);

/**
 * @brief 清空所有缓存数据（包括已上传和未上传）
 * @return true 成功 false 失败
 */
bool clearAllPendingData();

// ========== 工具函数 ==========

/**
 * @brief 获取缓存文件路径
 */
const char* getCacheFilePath();

/**
 * @brief 上传失败时延后第一条待上传数据
 * @return true 成功 false 失败
 */
bool deferFirstPendingDataAfterFailure();

#endif
