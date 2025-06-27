#include "log_manager.h"
#include <SPIFFS.h>
#include <time.h>

// 日志文件名
static const char* LOG_FILENAME = "/log.txt";

// 默认的配置
static LogLevel s_minLogLevel = LogLevel::DEBUG; // 默认写所有等级
static size_t   s_maxLogSize = 50 * 1024;        // 50KB

//------------------------------------------------
// 取日志等级对应字符串
//------------------------------------------------
static const char* levelName(LogLevel lvl) {
	switch (lvl) {
	case LogLevel::DEBUG: return "DEBUG";
	case LogLevel::INFO:  return "INFO";
	case LogLevel::WARN:  return "WARN";
	case LogLevel::ERROR: return "ERROR";
	default: return "UNKNOWN";
	}
}

//------------------------------------------------
// 获取当前本地时间字符串
// 需要设备已通过 NTP 同步，getLocalTime() 才能返回正确时间
//------------------------------------------------
static String getTimeString() {
	struct tm timeinfo;
	if (!getLocalTime(&timeinfo)) {
		return "1970-01-01 00:00:00";
	}
	char buf[20];
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
	return String(buf);
}

//------------------------------------------------
// 检查并限制日志文件大小
// 如果超限，删除并重建空的日志文件
//------------------------------------------------
static void checkLogSizeLimit() {
	File f = SPIFFS.open(LOG_FILENAME, FILE_READ);
	if (!f) {
		// 文件不存在则不处理
		return;
	}
	size_t sz = f.size();
	f.close();

	if (sz > s_maxLogSize) {
		if (SPIFFS.remove(LOG_FILENAME)) {
			Serial.println("[Log] /log.txt removed due to size limit.");
		}
		else {
			Serial.println("[Log] Failed to remove /log.txt!");
		}

		File newFile = SPIFFS.open(LOG_FILENAME, FILE_WRITE);
		if (newFile) {
			Serial.println("[Log] New empty /log.txt created.");
			newFile.close();
		}
		else {
			Serial.println("[Log] Failed to create new /log.txt!");
		}
	}
}

//------------------------------------------------
// 初始化日志系统
//------------------------------------------------
bool initLogSystem() {
	if (!SPIFFS.begin(true)) {
		Serial.println("[Log] SPIFFS mount fail!");
		return false;
	}
	Serial.println("[Log] SPIFFS mounted OK");

	// 确保日志文件存在
	if (!SPIFFS.exists(LOG_FILENAME)) {
		File f = SPIFFS.open(LOG_FILENAME, FILE_WRITE);
		if (f) {
			Serial.println("[Log] Created initial /log.txt");
			f.close();
		}
		else {
			Serial.println("[Log] Failed to create /log.txt on init!");
		}
	}
	return true;
}

//------------------------------------------------
// 设置最小日志等级
//------------------------------------------------
void setMinLogLevel(LogLevel level) {
	s_minLogLevel = level;
}

//------------------------------------------------
// 设置最大日志大小
//------------------------------------------------
void setMaxLogSize(size_t bytes) {
	s_maxLogSize = bytes;
}

//------------------------------------------------
// 写日志
//------------------------------------------------
bool logWrite(LogLevel level, const String& message) {
	// 1) 判断日志等级
	if (static_cast<int>(level) < static_cast<int>(s_minLogLevel)) {
		return true; // 不写入
	}

	// 2) 检查文件大小限制
	checkLogSizeLimit();

	// 3) 确保文件存在
	if (!SPIFFS.exists(LOG_FILENAME)) {
		File f = SPIFFS.open(LOG_FILENAME, FILE_WRITE);
		if (f) f.close();
	}

	// 4) 打开文件追加写入
	File file = SPIFFS.open(LOG_FILENAME, FILE_APPEND);
	if (!file) {
		Serial.println("[Log] open /log.txt for append fail!");
		return false;
	}

	// 5) 拼装日志内容
	String timeStr = getTimeString();
	String lvlName = levelName(level);
	String line = "[" + timeStr + "] [" + lvlName + "] " + message;

	// 6) 写入文件
	size_t written = file.println(line);
	file.close();

	if (written == 0) {
		Serial.println("[Log] write fail!");
		return false;
	}
	return true;
}

//------------------------------------------------
// 读取全部日志内容
//------------------------------------------------
String readAllLogs() {
	File file = SPIFFS.open(LOG_FILENAME, FILE_READ);
	if (!file) {
		return String("");
	}
	String content = file.readString();
	file.close();
	return content;
}
