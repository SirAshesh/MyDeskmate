#pragma once

#include <lvgl.h>
#include <stddef.h>

void loadTouchCal();
void resetTouchCal();
void getTouchCalString(char* buf, size_t len);
bool readTouchRaw(int& rawX, int& rawY, int& pressure);
void runTouchCalibration();
void initTouchDebugOverlay();
void lvglTouchReadCb(lv_indev_drv_t* drv, lv_indev_data_t* data);
