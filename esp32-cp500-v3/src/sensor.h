#pragma once
#include <Arduino.h>
#include <vector>

// ========== 初始化 ==========
bool initSensors(int tempInPin, int tempOutPin, int heaterPin, int pumpPin, int aerationPin);

// ========== 温度读取 ==========
float readTempIn();                           // 内部核心温度（内总线 index=0）
float readTempTank();                         // 水箱温度（内总线 index=1；不足则返回 NAN）
std::vector<float> readTempOut();             // 外浴多个探头（最多取前三个）

// ========== 加热 / 循环泵（数字开关） ==========
void heaterOn();
void heaterOff();
void pumpOn();
void pumpOff();

// ========== 曝气（PWM，内置软启停） ==========
// 一键软启动到 100%（或到 MaxDuty 限制）
void aerationOn();
// 一键软停止到 0%
void aerationOff();
// 当前是否 >0% 占空
bool aerationIsActive();

// 直接设置占空（硬切，不走软启动），范围 0~100
void aerationSetDutyPct(int pct);

// 限制最大占空（默认 100%）
void aerationSetMaxDutyPct(int pctLimit);     // 10~100

// 运行期配置软启动参数（默认 on=1200ms, off=800ms；可选起动脉冲 kick）
void aerationConfigSoft(int onMs, int offMs, int kickPct, int kickMs);
