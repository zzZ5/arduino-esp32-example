#include "emergency_stop.h"
#include "sensor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ========================= 急停模块 =========================

// 静态变量：互斥量保护状态访问
static SemaphoreHandle_t gEmergencyMutex = nullptr;

// 当前急停状态
static EmergencyState gEmergencyState = EMERGENCY_STATE_NORMAL;

// 急停模块初始化
void initEmergencyStop() {
  if (gEmergencyMutex == nullptr) {
    gEmergencyMutex = xSemaphoreCreateMutex();
  }
  gEmergencyState = EMERGENCY_STATE_NORMAL;
  Serial.println("[Emergency] 模块初始化完成");
}

// 检查是否处于急停状态
bool isEmergencyStopped() {
  if (gEmergencyMutex == nullptr) return false;

  bool stopped = false;
  if (xSemaphoreTake(gEmergencyMutex, pdMS_TO_TICKS(100))) {
    stopped = (gEmergencyState == EMERGENCY_STATE_STOPPED ||
               gEmergencyState == EMERGENCY_STATE_LOCKED);
    xSemaphoreGive(gEmergencyMutex);
  }
  return stopped;
}

// 激活紧急停止
// 立即关闭所有设备（加热器、水泵、曝气）并锁定系统
void activateEmergencyStop() {
  if (gEmergencyMutex == nullptr) {
    Serial.println("[Emergency] ❌ 互斥量未初始化");
    return;
  }

  // 阻塞式获取互斥量（优先级最高，可以阻塞）
  if (xSemaphoreTake(gEmergencyMutex, pdMS_TO_TICKS(1000))) {
    // 记录旧状态用于日志
    EmergencyState oldState = gEmergencyState;
    gEmergencyState = EMERGENCY_STATE_STOPPED;
    xSemaphoreGive(gEmergencyMutex);

    // 立即执行设备关闭操作（释放互斥量后再执行，避免死锁）
    Serial.println("[Emergency] 🛑 激活紧急停止！");
    if (oldState != EMERGENCY_STATE_STOPPED) {
      Serial.println("[Emergency] 关闭所有设备...");

      // 关闭加热器
      heaterOff();
      Serial.println("[Emergency]   - 加热器已关闭");

      // 关闭水泵
      pumpOff();
      Serial.println("[Emergency]   - 水泵已关闭");

      // 关闭曝气
      aerationOff();
      Serial.println("[Emergency]   - 曝气已关闭");

      Serial.println("[Emergency] 系统已锁定，等待启动指令");
    } else {
      Serial.println("[Emergency] 系统已处于急停状态");
    }
  } else {
    Serial.println("[Emergency] ❌ 无法获取互斥量，急停激活失败");
  }
}

// 解除急停并恢复系统运行
// 只有在急停状态下才能调用
void resumeFromEmergencyStop() {
  if (gEmergencyMutex == nullptr) {
    Serial.println("[Emergency] ❌ 互斥量未初始化");
    return;
  }

  if (xSemaphoreTake(gEmergencyMutex, pdMS_TO_TICKS(1000))) {
    if (gEmergencyState == EMERGENCY_STATE_STOPPED ||
        gEmergencyState == EMERGENCY_STATE_LOCKED) {
      gEmergencyState = EMERGENCY_STATE_NORMAL;
      xSemaphoreGive(gEmergencyMutex);

      Serial.println("[Emergency] ✅ 解除急停，系统恢复正常运行");
    } else {
      xSemaphoreGive(gEmergencyMutex);
      Serial.println("[Emergency] ⚠️ 系统未处于急停状态，无需恢复");
    }
  } else {
    Serial.println("[Emergency] ❌ 无法获取互斥量，恢复操作失败");
  }
}

// 在自动控制逻辑中检查急停状态
// 如果处于急停状态，阻止所有控制操作
bool shouldBlockControl() {
  if (gEmergencyMutex == nullptr) return false;

  bool block = false;
  if (xSemaphoreTake(gEmergencyMutex, pdMS_TO_TICKS(100))) {
    block = (gEmergencyState == EMERGENCY_STATE_STOPPED ||
             gEmergencyState == EMERGENCY_STATE_LOCKED);
    xSemaphoreGive(gEmergencyMutex);
  }
  return block;
}

// 获取当前急停状态
EmergencyState getEmergencyState() {
  if (gEmergencyMutex == nullptr) return EMERGENCY_STATE_NORMAL;

  EmergencyState state = EMERGENCY_STATE_NORMAL;
  if (xSemaphoreTake(gEmergencyMutex, pdMS_TO_TICKS(100))) {
    state = gEmergencyState;
    xSemaphoreGive(gEmergencyMutex);
  }
  return state;
}
