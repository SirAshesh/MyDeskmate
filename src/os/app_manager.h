#pragma once

#include "app_interface.h"

static constexpr int OS_MAX_APPS = 12;

void appManagerInit(lv_obj_t* panelLeft, lv_obj_t* panelRight, int rightW, int rightH);
int  appManagerRegister(const AppDescriptor& desc);
void appManagerOpen(int idx);
void appManagerLoop();

int                  appManagerGetCount();
int                  appManagerGetActiveIndex();
const AppDescriptor* appManagerGetDescriptor(int idx);

// Crash Guard (B): status karantina & pemulihan
bool        appManagerIsQuarantined(int idx);
int         appManagerGetQuarantineCount();
const char* appManagerGetLastCrashName();
const char* appManagerGetLastResetReasonStr();
void        appManagerUnquarantine(int idx);
void        appManagerClearAllQuarantine();
