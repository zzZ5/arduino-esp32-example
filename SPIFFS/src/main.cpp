#include <Arduino.h>
#include <SPIFFS.h>

// 仅查看 /log.txt
static const char* LOG_FILENAME = "/log.txt";

// 打印 /log.txt 内容
void printLogFile() {
  File file = SPIFFS.open(LOG_FILENAME, FILE_READ);
  if (!file) {
    Serial.println("[LogPrint] /log.txt doesn't exist or open fail!");
    return;
  }
  Serial.println("----- Start of /log.txt -----");
  while (file.available()) {
    Serial.write(file.read());
  }
  file.close();
  Serial.println("\n----- End of /log.txt -----");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n===== SPIFFS Log Viewer =====");

  // 挂载 SPIFFS (若不存在则自动格式化)
  if (!SPIFFS.begin(true)) {
    Serial.println("[FS] SPIFFS mount failed!");
    while (1) { delay(100); } // 卡死或改用别的措施
  }
  Serial.println("[FS] SPIFFS mounted OK.");

  // 读取并打印 /log.txt
  printLogFile();

  Serial.println("[Setup] Done, no further actions. Check output above for log content.");
}

void loop() {
  // 不做任何写操作或循环打印，如只想查看一次即可
  delay(1000);
}
