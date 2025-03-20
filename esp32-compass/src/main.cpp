/************************************************************
 * 结合四合一气体传感器 + 氣泵 + Wi-Fi + 多NTP(无限重试) + MQTT 上传示例
 *
 * 功能说明：
 *   1) 启动后连接 Wi-Fi + 无限循环 NTP 同步
 *   2) 连接 MQTT，增加缓冲区以支持较长 JSON
 *   3) 每 5 分钟 (READ_INTERVAL=300000) 执行一次采集：
 *      - 打开气泵 1 分钟，让传感器吸入空气
 *      - 读取四合一气体传感器 (CO/H2S/O2/CH4)
 *      - 拼装 JSON 并通过 MQTT 上传
 *      - 关闭气泵
 ************************************************************/

 #include <WiFi.h>
 #include <PubSubClient.h>
 #include <HardwareSerial.h>
 #include <time.h>
 
 //======== 1) Wi-Fi 配置 ========//
 const char* ssid     = "zzZ5";
 const char* password = "1450791278";
 
 //======== 2) MQTT 配置 ========//
 const char* mqttServer   = "118.25.108.254";
 const int   mqttPort     = 1883;
 const char* mqttUser     = "equipment";
 const char* mqttPass     = "ZNXK8888";
 const char* mqttClientId = "linhu";
 const char* mqttTopic    = "compostlab/test/post";
 
 //======== 3) 氣泵 & 采集周期设定 ========//
 // ---- 恢复正常工作参数 ----
 const unsigned long READ_INTERVAL = 300000;  // 5 分钟
 const int PUMP_RUN_TIME           = 60000;   // 1 分钟
 
 const int PUMP_PIN = 4; // 控制气泵的引脚(LOW=ON, HIGH=OFF)
 unsigned long previousTime = 0;
 
 //======== 4) 硬件串口 (四合一传感器) ========//
 HardwareSerial mySerial(1);
 #define RX_PIN 16
 #define TX_PIN 17
 
 //======== 5) 多个 NTP 服务器列表 & 时区设置 ========//
 const char* ntpServers[] = {
   "ntp.aliyun.com",
   "cn.ntp.org.cn",
   "ntp.tuna.tsinghua.edu.cn"
 };
 const int NTP_SERVER_COUNT = sizeof(ntpServers) / sizeof(ntpServers[0]);
 
 // 示例：UTC+8 (中国)
 const long gmtOffset_sec     = 8 * 3600;
 const int  daylightOffsetSec = 0;
 
 // 超时时间(毫秒)，单次 configTime() 后等待的同步时间
 const unsigned long NTP_TIMEOUT_MS = 5000;
 
 //============================================================
 //           全局对象: WiFi & MQTT
 //============================================================
 WiFiClient espClient;
 PubSubClient mqttClient(espClient);
 
 //============================================================
 //     1) 氣泵控制函数封装
 //============================================================
 void pumpOn() {
   digitalWrite(PUMP_PIN, LOW);
   Serial.println("Pump ON");
 }
 
 void pumpOff() {
   digitalWrite(PUMP_PIN, HIGH);
   Serial.println("Pump OFF");
 }
 
 //============================================================
 //     2) 四合一气体传感器初始化 / 读取
 //============================================================
 
 // 初始化(切换到问答模式)
 void initializeSensor() {
   Serial.println("初始化传感器 -> 切换问答模式...");
   uint8_t switchToQueryMode[] = {
     0xFF, 0x01, 0x78, 0x41,
     0x00, 0x00, 0x00, 0x00,
     0x46
   };
   mySerial.write(switchToQueryMode, sizeof(switchToQueryMode));
   delay(1000);
 }
 
 // 读取四合一传感器 (CO/H2S/O2/CH4)
 bool readSensorData(uint16_t &coVal,
                     uint16_t &h2sVal,
                     float    &o2Val,
                     uint16_t &ch4Val)
 {
   // 发送查询命令
   uint8_t queryCommand[] = {
     0xFF, 0x01, 0x86,
     0x00, 0x00, 0x00,
     0x00, 0x00, 0x79
   };
   mySerial.write(queryCommand, sizeof(queryCommand));
 
   // 接收 11 字节响应
   uint8_t response[11];
   unsigned long startTime = millis();
   uint8_t index = 0;
   while (index < 11 && (millis() - startTime) < 200) {
     if (mySerial.available()) {
       response[index++] = mySerial.read();
     }
   }
   if (index < 11) {
     Serial.println("数据帧读取不完整！");
     return false;
   }
 
   // 校验和
   uint8_t calcSum = 0;
   for (uint8_t i = 1; i < 10; i++) {
     calcSum += response[i];
   }
   calcSum = (~calcSum) + 1;
   if (calcSum != response[10]) {
     Serial.println("校验失败！");
     return false;
   }
 
   // 解析
   coVal  = (response[2] << 8) | response[3];
   h2sVal = (response[4] << 8) | response[5];
   o2Val  = ((response[6] << 8) | response[7]) * 0.1f;
   ch4Val = (response[8] << 8) | response[9];
 
   return true;
 }
 
 //============================================================
 //     3) Wi-Fi / MQTT 函数
 //============================================================
 
 // 连接 Wi-Fi
 void connectToWiFi() {
   WiFi.mode(WIFI_STA);
   WiFi.begin(ssid, password);
 
   Serial.print("连接Wi-Fi: ");
   Serial.println(ssid);
 
   while (WiFi.status() != WL_CONNECTED) {
     delay(500);
     Serial.print(".");
   }
   Serial.print("\nWiFi 已连接, IP: ");
   Serial.println(WiFi.localIP());
 }
 
 // 等待 NTP 时间同步
 bool waitForSync(unsigned long timeoutMs) {
   unsigned long start = millis();
   struct tm timeinfo;
   while ((millis() - start) < timeoutMs) {
     if (getLocalTime(&timeinfo)) {
       return true;
     }
     delay(100);
   }
   return false;
 }
 
 // 多NTP无限重试
 void multiNTPSetup() {
   bool timeSynced = false;
 
   while (!timeSynced) {
     // 依次尝试多个 NTP
     for (int i = 0; i < NTP_SERVER_COUNT; i++) {
       Serial.print("Config NTP server: ");
       Serial.println(ntpServers[i]);
       configTime(gmtOffset_sec, daylightOffsetSec, ntpServers[i]);
 
       // 等待同步
       if (waitForSync(NTP_TIMEOUT_MS)) {
         Serial.println("NTP time sync success!");
         timeSynced = true;
         break;  // 退出 for
       } else {
         Serial.println("NTP sync failed with current server, trying next...");
       }
     }
 
     // 如果整个 for 循环都失败
     if (!timeSynced) {
       Serial.println("All NTP servers failed. Will retry in 5 seconds...");
       delay(5000);
     }
   }
 
   Serial.println("NTP setup done, time is synced!");
 }
 
 // 连接 MQTT (增大发送缓冲区以支持较大 JSON)
 void connectToMQTT() {
   mqttClient.setServer(mqttServer, mqttPort);
   mqttClient.setBufferSize(512);  // 调整发送缓冲区大小
   while (!mqttClient.connected()) {
     Serial.print("连接到 MQTT: ");
     if (mqttClient.connect(mqttClientId, mqttUser, mqttPass)) {
       Serial.println("成功!");
       // 给一点时间处理握手
       unsigned long t0 = millis();
       while (millis() - t0 < 200) {
         mqttClient.loop();
         delay(10);
       }
     } else {
       Serial.print("失败, 错误码: ");
       Serial.println(mqttClient.state());
       Serial.println("2秒后重试...");
       delay(2000);
     }
   }
 }
 
 // 发送数据到 MQTT
 void publishData(const String &payload) {
   if (!mqttClient.connected()) {
     Serial.println("MQTT not connected, trying reconnect...");
     connectToMQTT();
 
     // 连接刚成功，再给一点时间让客户端初始化
     unsigned long t0 = millis();
     while (millis() - t0 < 200) {
       mqttClient.loop();
       delay(10);
     }
   }
 
   bool ok = mqttClient.publish(mqttTopic, payload.c_str());
   if (ok) {
     Serial.println("MQTT 发布成功: " + payload);
   } else {
     Serial.println("MQTT 发布失败!");
     Serial.print("mqttClient.state() = ");
     Serial.println(mqttClient.state());
   }
 }
 
 //============================================================
 //     4) 获取当前时间的字符串
 //============================================================
 String getTimeString() {
   struct tm timeinfo;
   if (!getLocalTime(&timeinfo)) {
     // 若获取失败,返回占位值
     return "1970-01-01 00:00:00";
   }
   char buf[20];
   strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
   return String(buf);
 }
 
 //============================================================
 //     setup() / loop()
 //============================================================
 void setup() {
   Serial.begin(115200);
 
   //--- 1) 连接 Wi-Fi
   connectToWiFi();
 
   //--- 2) 多NTP无限重试，直到成功
   multiNTPSetup();
 
   //--- 3) 连接 MQTT
   connectToMQTT();
 
   //--- 4) 传感器串口 & 初始化
   mySerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
   initializeSensor();
 
   //--- 5) 氣泵引脚初始化 (注意默认关闭)
   pinMode(PUMP_PIN, OUTPUT);
   pumpOff();
 
   Serial.println("Setup 完成. 进入主循环...");
 }
 
 void loop() {
   // 保持 MQTT 心跳
   if (!mqttClient.connected()) {
     Serial.println("loop: MQTT not connected, trying reconnect...");
     connectToMQTT();
   }
   mqttClient.loop();
 
   // 每 READ_INTERVAL ms(5分钟) 进行一次数据采集 & 发布
   unsigned long currentTime = millis();
   if (currentTime - previousTime >= READ_INTERVAL) {
     previousTime = currentTime;
 
     //=== (1) 打开气泵 1分钟
     pumpOn();
     Serial.println("Pump is ON, waiting 60s to gather air...");
     delay(PUMP_RUN_TIME);
 
     //=== (2) 读取传感器
     uint16_t coVal, h2sVal, ch4Val;
     float o2Val;
     if (readSensorData(coVal, h2sVal, o2Val, ch4Val)) {
       Serial.printf("CO=%u ppm, H2S=%u ppm, O2=%.1f %%VOL, CH4=%u %%LEL\n",
                     coVal, h2sVal, o2Val, ch4Val);
 
       //=== (3) 获取当前时间(字符串)
       String measuredTime = getTimeString();
 
       //=== (4) 组装 JSON
       // {
       //   "data": [
       //     { "value":..., "key":"CO",  "measured_time":"..." },
       //     { "value":..., "key":"H2S", "measured_time":"..." },
       //     { "value":..., "key":"O2",  "measured_time":"..." },
       //     { "value":..., "key":"CH4", "measured_time":"..." }
       //   ]
       // }
       String payload = "{";
       payload += "\"data\":[";
 
       // CO
       payload += "{";
       payload += "\"value\":" + String(coVal);
       payload += ",\"key\":\"CO\"";
       payload += ",\"measured_time\":\"" + measuredTime + "\"";
       payload += "},";
 
       // H2S
       payload += "{";
       payload += "\"value\":" + String(h2sVal);
       payload += ",\"key\":\"H2S\"";
       payload += ",\"measured_time\":\"" + measuredTime + "\"";
       payload += "},";
 
       // O2
       payload += "{";
       payload += "\"value\":" + String(o2Val, 1); // 保留1位小数
       payload += ",\"key\":\"O2\"";
       payload += ",\"measured_time\":\"" + measuredTime + "\"";
       payload += "},";
 
       // CH4
       payload += "{";
       payload += "\"value\":" + String(ch4Val);
       payload += ",\"key\":\"CH4\"";
       payload += ",\"measured_time\":\"" + measuredTime + "\"";
       payload += "}";
 
       payload += "]}";
 
       //=== (5) 发布到 MQTT
       publishData(payload);
     } else {
       Serial.println("读取传感器数据失败!");
     }
 
     //=== (6) 关闭气泵
     pumpOff();
   }
 
   // 短暂延时(让CPU有空闲，也可做其他逻辑)
   delay(100);
 }
 