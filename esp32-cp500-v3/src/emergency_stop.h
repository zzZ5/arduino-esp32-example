#ifndef EMERGENCY_STOP_H
#define EMERGENCY_STOP_H

#include <Arduino.h>
#include <stdbool.h>

// 急停状态枚举
enum EmergencyState {
  EMERGENCY_STATE_NORMAL = 0,  // 正常运行状态
  EMERGENCY_STATE_STOPPED = 1, // 急停激活状态
  EMERGENCY_STATE_LOCKED = 2   // 锁定状态（等待启动指令）
};

// 急停模块初始化
void initEmergencyStop();

// 检查是否处于急停状态
bool isEmergencyStopped();

// 激活紧急停止
// 立即关闭所有设备（加热器、水泵、曝气）并锁定系统
void activateEmergencyStop();

// 解除急停并恢复系统运行
// 只有在急停状态下才能调用
void resumeFromEmergencyStop();

// 在自动控制逻辑中检查急停状态
// 如果处于急停状态，阻止所有控制操作
bool shouldBlockControl();

// 获取当前急停状态
EmergencyState getEmergencyState();

#endif // EMERGENCY_STOP_H
