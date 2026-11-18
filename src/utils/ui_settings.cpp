#include "ui_settings.h"
#include <Arduino.h>
#include <Preferences.h>

static constexpr int MAX_LISTENERS = 12;
static UiSettings s_cfg = { 100, 8, true, true, true, false };
static UiSettingsListener s_listeners[MAX_LISTENERS] = {};
static int s_listenerCount = 0;

const UiSettings& uiSettingsGet() {
  return s_cfg;
}

void uiSettingsLoad() {
  Preferences prefs;
  if (prefs.begin("ui_cfg", true)) {
    s_cfg.scalePct        = constrain(prefs.getInt("scale", 100), 60, 110);
    s_cfg.rowGapPx        = constrain(prefs.getInt("gap", 8), 2, 24);
    s_cfg.showSeconds     = prefs.getBool("sec", true);
    s_cfg.showDate        = prefs.getBool("date", true);
    s_cfg.showTouchDot    = prefs.getBool("touchDot", true);
    s_cfg.hideTouchCoords = prefs.getBool("hideTouch", false);
    prefs.end();
  }
}

void uiSettingsSave() {
  Preferences prefs;
  if (prefs.begin("ui_cfg", false)) {
    prefs.putInt("scale", s_cfg.scalePct);
    prefs.putInt("gap", s_cfg.rowGapPx);
    prefs.putBool("sec", s_cfg.showSeconds);
    prefs.putBool("date", s_cfg.showDate);
    prefs.putBool("touchDot", s_cfg.showTouchDot);
    prefs.putBool("hideTouch", s_cfg.hideTouchCoords);
    prefs.end();
  }
}

void uiSettingsReset() {
  s_cfg = { 100, 8, true, true, true, false };
  uiSettingsSave();
  uiSettingsNotifyAll();
}

void uiSettingsUpdate(const UiSettings& nextCfg, bool persist) {
  s_cfg.scalePct        = constrain(nextCfg.scalePct, 60, 110);
  s_cfg.rowGapPx        = constrain(nextCfg.rowGapPx, 2, 24);
  s_cfg.showSeconds     = nextCfg.showSeconds;
  s_cfg.showDate        = nextCfg.showDate;
  s_cfg.showTouchDot    = nextCfg.showTouchDot;
  s_cfg.hideTouchCoords = nextCfg.hideTouchCoords;
  if (persist) {
    uiSettingsSave();
  }
  uiSettingsNotifyAll();
}

void uiSettingsSubscribe(UiSettingsListener listener) {
  if (!listener || s_listenerCount >= MAX_LISTENERS) return;
  for (int i = 0; i < s_listenerCount; i++) {
    if (s_listeners[i] == listener) return;
  }
  s_listeners[s_listenerCount++] = listener;
}

void uiSettingsNotifyAll() {
  for (int i = 0; i < s_listenerCount; i++) {
    if (s_listeners[i]) {
      s_listeners[i](s_cfg);
    }
  }
}

int uiScalePx(int basePx) {
  int scaled = (basePx * s_cfg.scalePct + 50) / 100;
  return (basePx > 0 && scaled < 1) ? 1 : scaled;
}

const lv_font_t* uiPickMontserratFont(int baseSizePx) {
  const int target = uiScalePx(baseSizePx);
  struct FontEntry {
    int size;
    const lv_font_t* font;
  };
  static const FontEntry kFonts[] = {
    { 12, &lv_font_montserrat_12 },
    { 14, &lv_font_montserrat_14 },
    { 16, &lv_font_montserrat_16 },
    { 20, &lv_font_montserrat_20 },
    { 24, &lv_font_montserrat_24 },
    { 28, &lv_font_montserrat_28 },
    { 48, &lv_font_montserrat_48 },
  };

  const lv_font_t* best = kFonts[0].font;
  int bestDiff = abs(target - kFonts[0].size);
  for (size_t i = 1; i < sizeof(kFonts) / sizeof(kFonts[0]); i++) {
    int d = abs(target - kFonts[i].size);
    if (d < bestDiff) {
      bestDiff = d;
      best = kFonts[i].font;
    }
  }
  return best;
}
