#pragma once
#include <stddef.h>

void mqttNotifyLoop();
void mqttNotifyGetStatus(char* buf, size_t len);
void mqttNotifySendDismiss(const char* key);
void mqttNotifySendClearAll();
