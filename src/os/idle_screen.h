#pragma once

#include <lvgl.h>

void idleScreenInit();
void idleScreenNotifyActivity();
bool idleScreenIsActive();
void idleScreenEnter();
void idleScreenExit();
