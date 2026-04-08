#pragma once
#include <Arduino.h>
#include <vector>

// ========== 初始化 ==========
bool initSensors(int tempInPin, int tempOutPin, int heaterPin, int pumpPin, int aerationPin);

// ========== 温度读取 ==========
float readTempIn();                           // 内部核心温度（内总线 index=0）
float readTempTank();                         // 水箱温度（内总线 index=1；不足则返回 NAN）
void readInternalTemps(float& tempIn, float& tempTank); // 批量读取内部总线，避免重复转换
std::vector<float> readTempOut();             // 外浴多个探头（最多取前三个）

// ========== 加热 / 循环泵（数字开关） ==========
void heaterOn();
void heaterOff();
void pumpOn();
void pumpOff();

// ========== 曝气（PWM，内置软启停） ==========
void aerationOn();
void aerationOff();
bool aerationIsActive();
void aerationSetDutyPct(int pct);
void aerationSetMaxDutyPct(int pctLimit);     // 10~100
void aerationConfigSoft(int onMs, int offMs, int kickPct, int kickMs);
