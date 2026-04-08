#include "emergency_stop.h"
#include "sensor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Protect emergency state transitions across tasks.
static SemaphoreHandle_t gEmergencyMutex = nullptr;
static EmergencyState gEmergencyState = EMERGENCY_STATE_NORMAL;

void initEmergencyStop() {
  if (gEmergencyMutex == nullptr) {
    gEmergencyMutex = xSemaphoreCreateMutex();
  }
  gEmergencyState = EMERGENCY_STATE_NORMAL;
  Serial.println("[Emergency] Module initialized");
}

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

void activateEmergencyStop() {
  if (gEmergencyMutex == nullptr) {
    Serial.println("[Emergency] Mutex not initialized");
    return;
  }

  if (xSemaphoreTake(gEmergencyMutex, pdMS_TO_TICKS(1000))) {
    EmergencyState oldState = gEmergencyState;
    gEmergencyState = EMERGENCY_STATE_STOPPED;
    xSemaphoreGive(gEmergencyMutex);

    Serial.println("[Emergency] Emergency stop activated");
    if (oldState != EMERGENCY_STATE_STOPPED) {
      Serial.println("[Emergency] Shutting down all actuators...");

      heaterOff();
      Serial.println("[Emergency]   - Heater off");

      pumpOff();
      Serial.println("[Emergency]   - Pump off");

      aerationOff();
      Serial.println("[Emergency]   - Aeration off");

      Serial.println("[Emergency] System locked until resume command");
    } else {
      Serial.println("[Emergency] System already in emergency-stop state");
    }
  } else {
    Serial.println("[Emergency] Failed to acquire mutex for stop request");
  }
}

void resumeFromEmergencyStop() {
  if (gEmergencyMutex == nullptr) {
    Serial.println("[Emergency] Mutex not initialized");
    return;
  }

  if (xSemaphoreTake(gEmergencyMutex, pdMS_TO_TICKS(1000))) {
    if (gEmergencyState == EMERGENCY_STATE_STOPPED ||
        gEmergencyState == EMERGENCY_STATE_LOCKED) {
      gEmergencyState = EMERGENCY_STATE_NORMAL;
      xSemaphoreGive(gEmergencyMutex);
      Serial.println("[Emergency] Emergency stop cleared");
    } else {
      xSemaphoreGive(gEmergencyMutex);
      Serial.println("[Emergency] Resume ignored: system is not stopped");
    }
  } else {
    Serial.println("[Emergency] Failed to acquire mutex for resume request");
  }
}

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

EmergencyState getEmergencyState() {
  if (gEmergencyMutex == nullptr) return EMERGENCY_STATE_NORMAL;

  EmergencyState state = EMERGENCY_STATE_NORMAL;
  if (xSemaphoreTake(gEmergencyMutex, pdMS_TO_TICKS(100))) {
    state = gEmergencyState;
    xSemaphoreGive(gEmergencyMutex);
  }
  return state;
}
