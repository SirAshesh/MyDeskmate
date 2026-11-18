#pragma once

#include <lvgl.h>

lv_obj_t* buildAntigravityPage(lv_obj_t* parent, int rightW, int rightH);
void      antigravityHandleSerialLine(const char* line);
