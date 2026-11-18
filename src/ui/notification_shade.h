#pragma once

#include <lvgl.h>
#include <stdint.h>
#include "studio_theme.h"

void notificationShadeInit();
void notificationShadeOpen();
void notificationShadeClose();
bool notificationShadeIsOpen();

void notificationPush(const char* title,
                      const char* body,
                      const char* timeStr = nullptr,
                      uint32_t accentHex = StudioTheme::CHAMPAGNE,
                      const char* syncKey = nullptr);
bool notificationRemoveByKey(const char* syncKey);
void notificationClearAll(bool syncToPhone = true);
int  notificationCount();
