#pragma once

#include <lvgl.h>

void buildUi();
void uiSetActivePage(int pageIdx);
int  uiRegisterMenuPage(const char* symbol, lv_obj_t* page);
int  uiRegisterMenuPageImg(const lv_img_dsc_t* imgSrc, lv_obj_t* page);
lv_obj_t* uiGetPanelRight();
