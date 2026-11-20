#pragma once

#include <stdint.h>

void pcMouseInit();
void pcMouseLoop();
bool pcMouseGetState(int16_t& outX, int16_t& outY, bool& outLeftDown);
void pcMouseSetEnabled(bool enabled);
bool pcMouseIsEnabled();
bool pcMouseIsActive();
