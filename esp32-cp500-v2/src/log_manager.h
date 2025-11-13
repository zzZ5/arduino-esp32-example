#ifndef LOG_MANAGER_H
#define LOG_MANAGER_H

#include <Arduino.h>

/**
 * 定义日志等级
 */
enum class LogLevel {
	DEBUG = 0,
	INFO,
	WARN,
	ERROR
};

/**
 * 初始化日志系统（挂载 SPIFFS，或在外部先挂载也行）。
 * 若已在外部 initSPIFFS()，可省略此函数内部 SPIFFS.begin(true)
 */
bool initLogSystem();

/**
 * 设置日志等级（低于此的日志不会写入文件）
 */
void setMinLogLevel(LogLevel level);

/**
 * 设置日志文件的最大大小（单位：字节）
 * 超过后将执行轮转或删除
 */
void setMaxLogSize(size_t bytes);

/**
 * 写日志到 /log.txt：
 * - [时间戳] [等级名] message
 * - 如果日志文件超过限制，会删除或轮转(示例中做简单删除)
 * @param level   日志等级
 * @param message 要写入的内容
 * @return        true写入成功, false写入失败
 */
bool logWrite(LogLevel level, const String& message);

/**
 * 读取全部日志内容（仅供调试）
 */
String readAllLogs();

#endif
