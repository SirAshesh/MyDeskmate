#include "display_hx8357d.h"
#include "ui_config.h"

const int DATA_PINS[8] = {16, 17, 18, 8, 9, 10, 11, 13}; // D0..D7
LGFX_HX8357D tft;

LGFX_HX8357D::Panel_HX8357D::Panel_HX8357D() {
  _cfg.memory_width  = _cfg.panel_width  = PANEL_W;
  _cfg.memory_height = _cfg.panel_height = PANEL_H;
  _cfg.readable = false;
}

const uint8_t* LGFX_HX8357D::Panel_HX8357D::getInitCommands(uint8_t listno) const {
  static constexpr uint8_t list0[] = {
    CMD_SWRESET, 0 + CMD_INIT_DELAY, 120,
    CMD_SLPOUT,  0 + CMD_INIT_DELAY, 120,
    CMD_DISPON,  0 + CMD_INIT_DELAY, 50,
    0xFF, 0xFF,
  };
  return (listno == 0) ? list0 : nullptr;
}

uint8_t LGFX_HX8357D::Panel_HX8357D::getMadCtl(uint8_t r) const {
  static constexpr uint8_t table[] = {
    MAD_MX,
    MAD_MV,
    MAD_MY,
    MAD_MV | MAD_MX | MAD_MY,
    MAD_MX | MAD_MY,
    MAD_MV | MAD_MX,
    0,
    MAD_MV | MAD_MY,
  };
  return table[r & 7];
}

LGFX_HX8357D::LGFX_HX8357D() {
  {
    auto cfg = _bus.config();
    cfg.port       = 0;
    cfg.freq_write = 20000000;
    cfg.pin_rd     = PIN_RD;
    cfg.pin_wr     = PIN_WR;
    cfg.pin_rs     = PIN_RS;
    cfg.pin_d0     = DATA_PINS[0];
    cfg.pin_d1     = DATA_PINS[1];
    cfg.pin_d2     = DATA_PINS[2];
    cfg.pin_d3     = DATA_PINS[3];
    cfg.pin_d4     = DATA_PINS[4];
    cfg.pin_d5     = DATA_PINS[5];
    cfg.pin_d6     = DATA_PINS[6];
    cfg.pin_d7     = DATA_PINS[7];
    _bus.config(cfg);
    _panel.setBus(&_bus);
  }
  {
    auto cfg = _panel.config();
    cfg.pin_cs          = PIN_CS;
    cfg.pin_rst         = PIN_RST;
    cfg.pin_busy        = -1;
    cfg.memory_width    = PANEL_W;
    cfg.memory_height   = PANEL_H;
    cfg.panel_width     = PANEL_W;
    cfg.panel_height    = PANEL_H;
    cfg.offset_x        = 0;
    cfg.offset_y        = 0;
    cfg.offset_rotation = 0;
    cfg.invert          = true;
    cfg.rgb_order       = false; // MAD_BGR -> 0x48 / 0x28
    cfg.dlen_16bit      = false;
    cfg.bus_shared      = false;
    _panel.config(cfg);
  }
  setPanel(&_panel);
}

static void slowWriteByte(uint8_t val) {
  for (int i = 0; i < 8; i++) {
    digitalWrite(DATA_PINS[i], (val >> i) & 0x01);
  }
  digitalWrite(PIN_WR, LOW);
  delayMicroseconds(1);
  digitalWrite(PIN_WR, HIGH);
  delayMicroseconds(1);
}

static void slowCmd(uint8_t cmd) {
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_RS, LOW);
  slowWriteByte(cmd);
  digitalWrite(PIN_CS, HIGH);
}

static void slowData(uint8_t dat) {
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_RS, HIGH);
  slowWriteByte(dat);
  digitalWrite(PIN_CS, HIGH);
}

void hardwareSafePreInit() {
  const int ctrl[5] = {PIN_RD, PIN_WR, PIN_RS, PIN_CS, PIN_RST};
  for (int p : ctrl) {
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
  for (int p : DATA_PINS) pinMode(p, OUTPUT);

  digitalWrite(PIN_RST, HIGH); delay(20);
  digitalWrite(PIN_RST, LOW);  delay(100);
  digitalWrite(PIN_RST, HIGH); delay(150);

  slowCmd(0x01); delay(120);
  slowCmd(0x11); delay(120);
  slowCmd(0x3A); slowData(0x55);
  slowCmd(0x36); slowData(0x48);
  slowCmd(0x29); delay(50);
}

void displayInit() {
  hardwareSafePreInit();
  tft.init();
  tft.setRotation(UI_DISPLAY_ROTATION);
  tft.setColorDepth(16);
  tft.fillScreen(0x0841);
}

void lvglFlushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
  uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels((lgfx::rgb565_t*)&color_p->full, w * h);
  tft.endWrite();

  lv_disp_flush_ready(drv);
}
