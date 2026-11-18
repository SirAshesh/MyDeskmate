#include "touch_4wire.h"
#include "display_hx8357d.h"
#include "../ui/studio_theme.h"
#include "../utils/ui_settings.h"
#include "calibration_config.h"
#include "ui_config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <hal/gpio_ll.h>
#include <rom/gpio.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>

static constexpr int TS_XP = 11;     // D6
static constexpr int TS_XM = PIN_RS; // GPIO 6 (ADC1)
static constexpr int TS_YP = PIN_WR; // GPIO 7 (ADC1)
static constexpr int TS_YM = 13;     // D7
static constexpr int TS_MIN_PRESSURE = 450;

struct TouchCal {
  bool swapXY;
  int16_t xRaw0;
  int16_t xRaw1;
  int16_t yRaw0;
  int16_t yRaw1;
};

static TouchCal defaultTouchCalForRotation() {
  return { TOUCH_CAL_SWAP, TOUCH_CAL_X0, TOUCH_CAL_X1, TOUCH_CAL_Y0, TOUCH_CAL_Y1 };
}

static TouchCal touchCal = defaultTouchCalForRotation();

void getTouchCalString(char* buf, size_t len) {
  if (!buf || len == 0) return;
  snprintf(buf, len, "S:%d X:%d,%d Y:%d,%d",
           touchCal.swapXY ? 1 : 0,
           touchCal.xRaw0, touchCal.xRaw1,
           touchCal.yRaw0, touchCal.yRaw1);
}

void loadTouchCal() {
  Preferences prefs;
  touchCal = defaultTouchCalForRotation();
  if (!prefs.begin("s3os_touch", true)) {
    Serial.println("[CAL] NVS read-open failed (using default)");
    return;
  }
  bool valid = prefs.getBool("valid", false);
  uint8_t rot = prefs.getUChar("rot", 255);
  if (valid && rot == UI_DISPLAY_ROTATION) {
    touchCal.swapXY = prefs.getBool("swap", touchCal.swapXY);
    touchCal.xRaw0  = prefs.getShort("x0", touchCal.xRaw0);
    touchCal.xRaw1  = prefs.getShort("x1", touchCal.xRaw1);
    touchCal.yRaw0  = prefs.getShort("y0", touchCal.yRaw0);
    touchCal.yRaw1  = prefs.getShort("y1", touchCal.yRaw1);
    Serial.printf("[CAL] Loaded NVS: swap=%d x0=%d x1=%d y0=%d y1=%d\n",
                  touchCal.swapXY, touchCal.xRaw0, touchCal.xRaw1, touchCal.yRaw0, touchCal.yRaw1);
  } else {
    Serial.printf("[CAL] Using default (valid=%d rot=%d)\n", valid, rot);
  }
  prefs.end();
}

static void saveTouchCal() {
  Preferences prefs;
  if (!prefs.begin("s3os_touch", false)) {
    Serial.println("[CAL] ERROR: NVS write-open failed!");
    return;
  }
  size_t w = 0;
  w += prefs.putBool("valid", true);
  w += prefs.putUChar("rot", UI_DISPLAY_ROTATION);
  w += prefs.putBool("swap", touchCal.swapXY);
  w += prefs.putShort("x0", touchCal.xRaw0);
  w += prefs.putShort("x1", touchCal.xRaw1);
  w += prefs.putShort("y0", touchCal.yRaw0);
  w += prefs.putShort("y1", touchCal.yRaw1);
  prefs.end();
  Serial.printf("[CAL] Saved to NVS (%u bytes)\n", (unsigned)w);
}

void resetTouchCal() {
  Preferences prefs;
  prefs.begin("s3os_touch", false);
  prefs.clear();
  prefs.end();
  touchCal = defaultTouchCalForRotation();
}

static int readMedianAdc(int pin) {
  int s[5];
  for (int i = 0; i < 5; i++) {
    s[i] = analogRead(pin);
    delayMicroseconds(20);
  }
  for (int i = 0; i < 4; i++) {
    for (int j = i + 1; j < 5; j++) {
      if (s[j] < s[i]) {
        int t = s[i];
        s[i] = s[j];
        s[j] = t;
      }
    }
  }
  return s[2];
}

static void detachTouchPinsFromLcd() {
  gpio_matrix_out((gpio_num_t)PIN_RS, SIG_GPIO_OUT_IDX, 0, 0);
  gpio_matrix_out((gpio_num_t)PIN_WR, SIG_GPIO_OUT_IDX, 0, 0);
  gpio_matrix_out((gpio_num_t)TS_XP, SIG_GPIO_OUT_IDX, 0, 0);
  gpio_matrix_out((gpio_num_t)TS_YM, SIG_GPIO_OUT_IDX, 0, 0);
}

static void restoreTftBusPins() {
  pinMode(PIN_RS, OUTPUT);
  pinMode(PIN_WR, OUTPUT);
  pinMode(TS_XP, OUTPUT);
  pinMode(TS_YM, OUTPUT);
  gpio_ll_input_enable(&GPIO, (gpio_num_t)TS_XP);
  gpio_ll_input_enable(&GPIO, (gpio_num_t)TS_YM);
  gpio_matrix_out((gpio_num_t)PIN_RS, LCD_DC_IDX, 0, 0);
  gpio_matrix_out((gpio_num_t)PIN_WR, LCD_PCLK_IDX, 0, 0);
  gpio_matrix_out((gpio_num_t)DATA_PINS[6], LCD_DATA_OUT0_IDX + 6, 0, 0);
  gpio_matrix_out((gpio_num_t)DATA_PINS[7], LCD_DATA_OUT0_IDX + 7, 0, 0);
}

bool readTouchRaw(int& rawX, int& rawY, int& pressure) {
  tft.waitDisplay();
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  detachTouchPinsFromLcd();

  pinMode(TS_XP, OUTPUT); digitalWrite(TS_XP, LOW);
  pinMode(TS_YM, OUTPUT); digitalWrite(TS_YM, HIGH);
  pinMode(TS_XM, ANALOG);
  pinMode(TS_YP, ANALOG);
  delayMicroseconds(150);

  int z1 = analogRead(TS_XM);
  int z2 = analogRead(TS_YP);
  pressure = 4095 - (z2 - z1);

  if (pressure < TS_MIN_PRESSURE) {
    restoreTftBusPins();
    return false;
  }

  pinMode(TS_YM, INPUT);
  pinMode(TS_YP, ANALOG);
  pinMode(TS_XP, OUTPUT); digitalWrite(TS_XP, HIGH);
  pinMode(TS_XM, OUTPUT); digitalWrite(TS_XM, LOW);
  delayMicroseconds(150);
  rawX = readMedianAdc(TS_YP);

  pinMode(TS_XP, INPUT);
  pinMode(TS_XM, ANALOG);
  pinMode(TS_YP, OUTPUT); digitalWrite(TS_YP, HIGH);
  pinMode(TS_YM, OUTPUT); digitalWrite(TS_YM, LOW);
  delayMicroseconds(150);
  rawY = readMedianAdc(TS_XM);

  pinMode(TS_XP, OUTPUT); digitalWrite(TS_XP, LOW);
  pinMode(TS_YM, OUTPUT); digitalWrite(TS_YM, HIGH);
  pinMode(TS_XM, ANALOG);
  pinMode(TS_YP, ANALOG);
  delayMicroseconds(80);
  int zCheck = 4095 - (analogRead(TS_YP) - analogRead(TS_XM));

  restoreTftBusPins();
  return zCheck >= TS_MIN_PRESSURE;
}

static bool readTouchScreen(int& sx, int& sy, int* outRx = nullptr, int* outRy = nullptr, int* outPz = nullptr) {
  int rx = 0, ry = 0, pz = 0;
  if (!readTouchRaw(rx, ry, pz)) return false;

  int ax = touchCal.swapXY ? ry : rx;
  int ay = touchCal.swapXY ? rx : ry;

  long mx = map(ax, touchCal.xRaw0, touchCal.xRaw1, 0, UI_W - 1);
  long my = map(ay, touchCal.yRaw0, touchCal.yRaw1, 0, UI_H - 1);

  sx = constrain((int)mx, 0, UI_W - 1);
  sy = constrain((int)my, 0, UI_H - 1);
  if (outRx) *outRx = rx;
  if (outRy) *outRy = ry;
  if (outPz) *outPz = pz;
  return true;
}

#if UI_DEBUG_TOUCH_POS
static lv_obj_t* s_touchDbgDot   = nullptr;
static lv_obj_t* s_touchDbgLabel = nullptr;

static void applyTouchDebugSettings(const UiSettings& cfg) {
  if (s_touchDbgDot && !cfg.showTouchDot) {
    lv_obj_add_flag(s_touchDbgDot, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_touchDbgLabel) {
    if (cfg.hideTouchCoords) {
      lv_obj_add_flag(s_touchDbgLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(s_touchDbgLabel, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void initTouchDebugOverlay() {
  lv_obj_t* top = lv_layer_top();

  s_touchDbgDot = lv_obj_create(top);
  lv_obj_remove_style_all(s_touchDbgDot);
  lv_obj_set_size(s_touchDbgDot, 12, 12);
  lv_obj_set_style_radius(s_touchDbgDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_touchDbgDot, lv_color_hex(StudioTheme::CORAL), 0);
  lv_obj_set_style_bg_opa(s_touchDbgDot, LV_OPA_70, 0);
  lv_obj_set_style_border_color(s_touchDbgDot, lv_color_hex(StudioTheme::TEXT), 0);
  lv_obj_set_style_border_width(s_touchDbgDot, 1, 0);
  lv_obj_clear_flag(s_touchDbgDot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_touchDbgDot, LV_OBJ_FLAG_HIDDEN);

  s_touchDbgLabel = lv_label_create(top);
  lv_label_set_text(s_touchDbgLabel, "TOUCH: -");
  lv_obj_set_style_text_font(s_touchDbgLabel, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_touchDbgLabel, lv_color_hex(StudioTheme::CORAL), 0);
  lv_obj_set_style_bg_color(s_touchDbgLabel, lv_color_hex(StudioTheme::HEADER), 0);
  lv_obj_set_style_bg_opa(s_touchDbgLabel, LV_OPA_80, 0);
  lv_obj_set_style_border_color(s_touchDbgLabel, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(s_touchDbgLabel, 1, 0);
  lv_obj_set_style_radius(s_touchDbgLabel, 6, 0);
  lv_obj_set_style_pad_hor(s_touchDbgLabel, 6, 0);
  lv_obj_set_style_pad_ver(s_touchDbgLabel, 3, 0);
  lv_obj_align(s_touchDbgLabel, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
  lv_obj_clear_flag(s_touchDbgLabel, LV_OBJ_FLAG_CLICKABLE);

  uiSettingsSubscribe(applyTouchDebugSettings);
  applyTouchDebugSettings(uiSettingsGet());
}
#else
void initTouchDebugOverlay() {}
#endif

void lvglTouchReadCb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
  (void)drv;
  static bool wasDown = false;
  static int16_t lastX = 0;
  static int16_t lastY = 0;

  int sx = 0, sy = 0, rx = 0, ry = 0, pz = 0;
  if (readTouchScreen(sx, sy, &rx, &ry, &pz)) {
    lastX = (int16_t)sx;
    lastY = (int16_t)sy;
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = lastX;
    data->point.y = lastY;

#if UI_DEBUG_TOUCH_POS
    const UiSettings& cfg = uiSettingsGet();
    if (s_touchDbgDot) {
      if (cfg.showTouchDot) {
        lv_obj_set_pos(s_touchDbgDot, sx - 6, sy - 6);
        lv_obj_clear_flag(s_touchDbgDot, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(s_touchDbgDot, LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (s_touchDbgLabel && !cfg.hideTouchCoords) {
      lv_label_set_text_fmt(s_touchDbgLabel, "X:%d Y:%d (%d,%d)", sx, sy, rx, ry);
    }
#endif

    if (!wasDown) {
      wasDown = true;
      Serial.printf("[TOUCH] x=%d y=%d (rawX=%d rawY=%d z=%d)\n", sx, sy, rx, ry, pz);
    }
  } else {
    wasDown = false;
    data->state = LV_INDEV_STATE_RELEASED;
    data->point.x = lastX;
    data->point.y = lastY;

#if UI_DEBUG_TOUCH_POS
    if (s_touchDbgDot) {
      lv_obj_add_flag(s_touchDbgDot, LV_OBJ_FLAG_HIDDEN);
    }
#endif
  }
}

static bool waitTouchPointRaw(int& outRx, int& outRy, uint32_t timeoutMs = 15000) {
  int rx = 0, ry = 0, pz = 0;
  uint32_t start = millis();
  while (readTouchRaw(rx, ry, pz)) {
    if (millis() - start > timeoutMs) return false;
    delay(20);
  }

  int hits = 0;
  long sumX = 0, sumY = 0;
  while (hits < 4) {
    if (millis() - start > timeoutMs) return false;
    if (readTouchRaw(rx, ry, pz)) {
      sumX += rx;
      sumY += ry;
      hits++;
    } else {
      hits = 0;
      sumX = 0;
      sumY = 0;
    }
    delay(25);
  }
  outRx = (int)(sumX / 4);
  outRy = (int)(sumY / 4);
  while (readTouchRaw(rx, ry, pz)) {
    if (millis() - start > timeoutMs) break;
    delay(20);
  }
  return true;
}

static void drawCrosshair(int x, int y, uint16_t col) {
  tft.drawCircle(x, y, 10, col);
  tft.drawFastHLine(x - 16, y, 33, col);
  tft.drawFastVLine(x, y - 16, 33, col);
}

void runTouchCalibration() {
  constexpr int P1X = 32, P1Y = 32;
  constexpr int P2X = UI_W - 32, P2Y = UI_H - 32;

  tft.startWrite();
  tft.fillScreen(0x0841);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(0xEF5D, 0x0841);
  tft.drawString("TOUCH CALIBRATION", UI_W / 2, UI_H / 2 - 14);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(0x8C71, 0x0841);
  tft.drawString("Tap crosshair kiri-atas (+)", UI_W / 2, UI_H / 2 + 12);
  drawCrosshair(P1X, P1Y, 0xD614);
  tft.endWrite();

  int rx1 = 0, ry1 = 0;
  if (!waitTouchPointRaw(rx1, ry1)) {
    Serial.println("[CAL] Timeout waiting tap 1 (cancelled)");
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    return;
  }

  tft.startWrite();
  tft.fillScreen(0x0841);
  tft.setFont(&fonts::Font2);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(0xEF5D, 0x0841);
  tft.drawString("TOUCH CALIBRATION", UI_W / 2, UI_H / 2 - 14);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(0x8C71, 0x0841);
  tft.drawString("Tap crosshair kanan-bawah (+)", UI_W / 2, UI_H / 2 + 12);
  drawCrosshair(P2X, P2Y, 0x8E37);
  tft.endWrite();

  int rx2 = 0, ry2 = 0;
  if (!waitTouchPointRaw(rx2, ry2)) {
    Serial.println("[CAL] Timeout waiting tap 2 (cancelled)");
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    return;
  }

  int dRx = abs(rx2 - rx1);
  int dRy = abs(ry2 - ry1);
  touchCal.swapXY = (dRy > dRx);

  int a1 = touchCal.swapXY ? ry1 : rx1;
  int a2 = touchCal.swapXY ? ry2 : rx2;
  int b1 = touchCal.swapXY ? rx1 : ry1;
  int b2 = touchCal.swapXY ? rx2 : ry2;

  float spanX = (float)(P2X - P1X);
  float spanY = (float)(P2Y - P1Y);
  touchCal.xRaw0 = (int16_t)(a1 - (a2 - a1) * (P1X / spanX));
  touchCal.xRaw1 = (int16_t)(a2 + (a2 - a1) * ((UI_W - 1 - P2X) / spanX));
  touchCal.yRaw0 = (int16_t)(b1 - (b2 - b1) * (P1Y / spanY));
  touchCal.yRaw1 = (int16_t)(b2 + (b2 - b1) * ((UI_H - 1 - P2Y) / spanY));

  saveTouchCal();
  Serial.printf("[CAL] swap=%d x0=%d x1=%d y0=%d y1=%d\n",
                touchCal.swapXY, touchCal.xRaw0, touchCal.xRaw1, touchCal.yRaw0, touchCal.yRaw1);

  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(nullptr);
}
