#ifndef EMERGENCY_STOP_H
#define EMERGENCY_STOP_H

#include <Arduino.h>
#include <stdbool.h>

// Emergency-stop state
enum EmergencyState {
  EMERGENCY_STATE_NORMAL = 0,
  EMERGENCY_STATE_STOPPED = 1,
  EMERGENCY_STATE_LOCKED = 2
};

void initEmergencyStop();
bool isEmergencyStopped();
void activateEmergencyStop();
void resumeFromEmergencyStop();
bool shouldBlockControl();
EmergencyState getEmergencyState();

#endif
