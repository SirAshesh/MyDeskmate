#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

static constexpr int PIN_RD  = 15;
static constexpr int PIN_WR  = 7;
static constexpr int PIN_RS  = 6;
static constexpr int PIN_CS  = 5;
static constexpr int PIN_RST = 4;

extern const int DATA_PINS[8];

static constexpr int PANEL_W = 320;
static constexpr int PANEL_H = 480;
static constexpr int UI_W    = 480;
static constexpr int UI_H    = 320;

class LGFX_HX8357D : public lgfx::LGFX_Device {
  struct Panel_HX8357D : public lgfx::Panel_ILI948x {
    Panel_HX8357D();
  protected:
    const uint8_t* getInitCommands(uint8_t listno) const override;
    uint8_t getMadCtl(uint8_t r) const override;
  };

  Panel_HX8357D       _panel;
  lgfx::Bus_Parallel8 _bus;

public:
  LGFX_HX8357D();
};

extern LGFX_HX8357D tft;

void hardwareSafePreInit();
void displayInit();
void lvglFlushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p);
