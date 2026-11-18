#include "page_timer.h"
#include "studio_theme.h"

#include <Arduino.h>
#include <stdio.h>

enum class TimerState {
  IDLE,
  RUNNING,
  PAUSED,
  FINISHED
};

static lv_obj_t* s_pageTimer     = nullptr;
static lv_obj_t* s_lblStatus     = nullptr;
static lv_obj_t* s_badgeStatus   = nullptr;
static lv_obj_t* s_lblTime       = nullptr;
static lv_obj_t* s_barRemain     = nullptr;
static lv_obj_t* s_lblSet        = nullptr;
static lv_obj_t* s_sldDuration   = nullptr;
static lv_obj_t* s_btnStartPause = nullptr;
static lv_obj_t* s_lblStartPause = nullptr;
static lv_obj_t* s_btnRepeat     = nullptr;
static lv_obj_t* s_btnStop       = nullptr;

static TimerState s_state       = TimerState::IDLE;
static uint32_t   s_durationMs  = 25UL * 60UL * 1000UL;
static uint32_t   s_remainMs    = 25UL * 60UL * 1000UL;
static uint32_t   s_lastTickMs  = 0;

static void formatHms(uint32_t totalMs, char* buf, size_t len) {
  uint32_t totalSec = (totalMs + 999UL) / 1000UL;
  if (totalMs == 0) totalSec = 0;
  uint32_t h = totalSec / 3600UL;
  uint32_t m = (totalSec % 3600UL) / 60UL;
  uint32_t s = totalSec % 60UL;
  snprintf(buf, len, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

static void refreshTimerUi() {
  if (!s_lblTime || !s_barRemain || !s_lblStatus || !s_badgeStatus || !s_lblStartPause) return;

  char timeBuf[16];
  formatHms(s_remainMs, timeBuf, sizeof(timeBuf));
  lv_label_set_text(s_lblTime, timeBuf);

  int32_t permille = 0;
  if (s_durationMs > 0) {
    permille = (int32_t)((uint64_t)s_remainMs * 1000ULL / s_durationMs);
    if (permille > 1000) permille = 1000;
    if (permille < 0) permille = 0;
  }
  lv_bar_set_value(s_barRemain, permille, LV_ANIM_OFF);

  uint32_t barCol = StudioTheme::TEAL;
  uint32_t txtCol = StudioTheme::TEXT;
  uint32_t badgeCol = StudioTheme::RULE;
  const char* statusTxt = "Siap";
  const char* btnTxt = LV_SYMBOL_PLAY " Mulai";
  uint32_t btnBorder = StudioTheme::TEAL;

  switch (s_state) {
    case TimerState::IDLE:
      statusTxt = "Siap";
      badgeCol  = StudioTheme::RULE;
      txtCol    = StudioTheme::TEXT;
      barCol    = StudioTheme::TEAL;
      btnTxt    = LV_SYMBOL_PLAY " Mulai";
      btnBorder = StudioTheme::TEAL;
      break;
    case TimerState::RUNNING:
      statusTxt = "Berjalan";
      badgeCol  = StudioTheme::TEAL;
      txtCol    = StudioTheme::CHAMPAGNE;
      barCol    = (permille <= 150) ? StudioTheme::ROSE :
                  (permille <= 350) ? StudioTheme::CORAL : StudioTheme::TEAL;
      btnTxt    = LV_SYMBOL_PAUSE " Jeda";
      btnBorder = StudioTheme::CORAL;
      break;
    case TimerState::PAUSED:
      statusTxt = "Dijeda";
      badgeCol  = StudioTheme::CORAL;
      txtCol    = StudioTheme::CHAMPAGNE;
      barCol    = StudioTheme::CORAL;
      btnTxt    = LV_SYMBOL_PLAY " Lanjut";
      btnBorder = StudioTheme::TEAL;
      break;
    case TimerState::FINISHED:
      statusTxt = "Selesai!";
      badgeCol  = StudioTheme::ROSE;
      txtCol    = StudioTheme::ROSE;
      barCol    = StudioTheme::ROSE;
      btnTxt    = LV_SYMBOL_PLAY " Mulai";
      btnBorder = StudioTheme::TEAL;
      break;
  }

  lv_obj_set_style_text_color(s_lblTime, lv_color_hex(txtCol), 0);
  lv_obj_set_style_bg_color(s_barRemain, lv_color_hex(barCol), LV_PART_INDICATOR);
  lv_label_set_text(s_lblStatus, statusTxt);
  lv_obj_set_style_border_color(s_badgeStatus, lv_color_hex(badgeCol), 0);
  lv_obj_set_style_text_color(s_lblStatus, lv_color_hex(
    s_state == TimerState::IDLE ? StudioTheme::MUTED : badgeCol), 0);
  lv_label_set_text(s_lblStartPause, btnTxt);
  if (s_btnStartPause) {
    lv_obj_set_style_border_color(s_btnStartPause, lv_color_hex(btnBorder), 0);
  }
  lv_obj_set_style_text_color(s_lblStartPause, lv_color_hex(btnBorder), 0);

  if (s_lblSet) {
    uint32_t mins = s_durationMs / 60000UL;
    lv_label_set_text_fmt(s_lblSet, "Durasi: %lu menit", (unsigned long)mins);
  }
}

static void setDurationMinutes(int mins, bool syncSlider) {
  if (mins < 1) mins = 1;
  if (mins > 120) mins = 120;
  s_state      = TimerState::IDLE;
  s_durationMs = (uint32_t)mins * 60UL * 1000UL;
  s_remainMs   = s_durationMs;
  if (syncSlider && s_sldDuration) {
    lv_slider_set_value(s_sldDuration, mins, LV_ANIM_OFF);
  }
  refreshTimerUi();
}

static void onTimerTick(lv_timer_t* timer) {
  (void)timer;
  if (s_state != TimerState::RUNNING) return;

  uint32_t now = millis();
  uint32_t dt  = now - s_lastTickMs;
  s_lastTickMs = now;

  if (dt >= s_remainMs) {
    s_remainMs = 0;
    s_state    = TimerState::FINISHED;
  } else {
    s_remainMs -= dt;
  }
  refreshTimerUi();
}

static void onStartPauseClick(lv_event_t* e) {
  (void)e;
  if (s_state == TimerState::RUNNING) {
    s_state = TimerState::PAUSED;
  } else {
    if (s_state == TimerState::FINISHED || s_remainMs == 0) {
      s_remainMs = s_durationMs;
    }
    s_lastTickMs = millis();
    s_state      = TimerState::RUNNING;
  }
  refreshTimerUi();
}

static void onRepeatClick(lv_event_t* e) {
  (void)e;
  s_remainMs   = s_durationMs;
  s_lastTickMs = millis();
  s_state      = TimerState::RUNNING;
  refreshTimerUi();
}

static void onStopResetClick(lv_event_t* e) {
  (void)e;
  s_state    = TimerState::IDLE;
  s_remainMs = s_durationMs;
  refreshTimerUi();
}

static void onDurationSliderChanged(lv_event_t* e) {
  lv_obj_t* slider = lv_event_get_target(e);
  int mins = lv_slider_get_value(slider);
  setDurationMinutes(mins, false);
}

static void onPresetClick(lv_event_t* e) {
  int mins = (int)(intptr_t)lv_event_get_user_data(e);
  setDurationMinutes(mins, true);
}

static lv_obj_t* makeActionBtn(lv_obj_t* parent, int w, int h, const char* text,
                               uint32_t accentHex, lv_event_cb_t cb, lv_obj_t** outLbl = nullptr) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::ELEVATED), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(accentHex), 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_radius(btn, 10, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(accentHex), 0);
  lv_obj_center(lbl);

  if (outLbl) *outLbl = lbl;
  return btn;
}

lv_obj_t* buildTimerPage(lv_obj_t* parent, int rightW, int rightH) {
  s_pageTimer = makePlainBox(parent, 0, 0, rightW, rightH);
  lv_obj_set_style_pad_hor(s_pageTimer, 14, 0);
  lv_obj_set_style_pad_ver(s_pageTimer, 12, 0);
  lv_obj_set_style_pad_row(s_pageTimer, 10, 0);
  lv_obj_set_layout(s_pageTimer, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_pageTimer, LV_FLEX_FLOW_COLUMN);

  const int contentW = rightW - 28;

  // Header
  lv_obj_t* hdr = makePlainBox(s_pageTimer, 0, 0, contentW, LV_SIZE_CONTENT);
  lv_obj_set_layout(hdr, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* lblTitle = lv_label_create(hdr);
  lv_label_set_text(lblTitle, "Timer");
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_badgeStatus = makePlainBox(hdr, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(s_badgeStatus, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_opa(s_badgeStatus, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_badgeStatus, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(s_badgeStatus, 1, 0);
  lv_obj_set_style_radius(s_badgeStatus, 8, 0);
  lv_obj_set_style_pad_hor(s_badgeStatus, 10, 0);
  lv_obj_set_style_pad_ver(s_badgeStatus, 3, 0);

  s_lblStatus = lv_label_create(s_badgeStatus);
  lv_label_set_text(s_lblStatus, "Siap");
  lv_obj_set_style_text_font(s_lblStatus, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblStatus, lv_color_hex(StudioTheme::MUTED), 0);

  // Main row: Kiri (Jam + ProgressBar + Slider), Kanan (Tombol aksi)
  constexpr int RIGHT_COL_W = 114;
  constexpr int COL_GAP     = 10;
  const int leftCardW       = contentW - RIGHT_COL_W - COL_GAP;
  constexpr int MAIN_H      = 196;

  lv_obj_t* mainRow = makePlainBox(s_pageTimer, 0, 0, contentW, MAIN_H);
  lv_obj_set_layout(mainRow, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(mainRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(mainRow, COL_GAP, 0);

  // Left card
  lv_obj_t* leftCard = makePlainBox(mainRow, 0, 0, leftCardW, MAIN_H);
  lv_obj_set_style_bg_color(leftCard, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_opa(leftCard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(leftCard, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(leftCard, 1, 0);
  lv_obj_set_style_radius(leftCard, 12, 0);
  lv_obj_set_style_pad_hor(leftCard, 14, 0);
  lv_obj_set_style_pad_ver(leftCard, 14, 0);
  lv_obj_set_style_pad_row(leftCard, 12, 0);
  lv_obj_set_layout(leftCard, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(leftCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(leftCard, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  s_lblTime = lv_label_create(leftCard);
  lv_label_set_text(s_lblTime, "00:25:00");
  lv_obj_set_style_text_font(s_lblTime, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(s_lblTime, lv_color_hex(StudioTheme::TEXT), 0);

  // Progressbar sisa waktu
  s_barRemain = lv_bar_create(leftCard);
  lv_obj_remove_style_all(s_barRemain);
  lv_obj_set_size(s_barRemain, lv_pct(100), 10);
  lv_bar_set_range(s_barRemain, 0, 1000);
  lv_bar_set_value(s_barRemain, 1000, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s_barRemain, lv_color_hex(StudioTheme::TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_barRemain, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_barRemain, 5, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_barRemain, lv_color_hex(StudioTheme::TEAL), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_barRemain, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(s_barRemain, 5, LV_PART_INDICATOR);

  // Label durasi + Slider atur durasi
  s_lblSet = lv_label_create(leftCard);
  lv_label_set_text(s_lblSet, "Durasi: 25 menit");
  lv_obj_set_style_text_font(s_lblSet, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblSet, lv_color_hex(StudioTheme::MUTED), 0);

  s_sldDuration = lv_slider_create(leftCard);
  lv_slider_set_range(s_sldDuration, 1, 120);
  lv_slider_set_value(s_sldDuration, 25, LV_ANIM_OFF);
  lv_obj_set_size(s_sldDuration, lv_pct(100), 8);
  lv_obj_set_style_bg_color(s_sldDuration, lv_color_hex(StudioTheme::TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_sldDuration, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_sldDuration, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_sldDuration, lv_color_hex(StudioTheme::CHAMPAGNE), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_sldDuration, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(s_sldDuration, 4, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_sldDuration, lv_color_hex(StudioTheme::TEXT), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(s_sldDuration, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_border_color(s_sldDuration, lv_color_hex(StudioTheme::CHAMPAGNE), LV_PART_KNOB);
  lv_obj_set_style_border_width(s_sldDuration, 2, LV_PART_KNOB);
  lv_obj_set_style_pad_all(s_sldDuration, 5, LV_PART_KNOB);
  lv_obj_add_event_cb(s_sldDuration, onDurationSliderChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  // Right column: Tombol Mulai/Jeda, Ulangi, Reset
  lv_obj_t* rightCol = makePlainBox(mainRow, 0, 0, RIGHT_COL_W, MAIN_H);
  lv_obj_set_layout(rightCol, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(rightCol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(rightCol, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  constexpr int BTN_H = 58;
  s_btnStartPause = makeActionBtn(rightCol, RIGHT_COL_W, BTN_H, LV_SYMBOL_PLAY " Mulai",
                                  StudioTheme::TEAL, onStartPauseClick, &s_lblStartPause);
  s_btnRepeat     = makeActionBtn(rightCol, RIGHT_COL_W, BTN_H, LV_SYMBOL_REFRESH " Ulangi",
                                  StudioTheme::CHAMPAGNE, onRepeatClick);
  s_btnStop       = makeActionBtn(rightCol, RIGHT_COL_W, BTN_H, LV_SYMBOL_STOP " Reset",
                                  StudioTheme::MUTED, onStopResetClick);

  // Bottom quick presets
  lv_obj_t* presetRow = makePlainBox(s_pageTimer, 0, 0, contentW, 38);
  lv_obj_set_layout(presetRow, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(presetRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(presetRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  static constexpr int kPresets[] = { 1, 5, 10, 15, 25, 45 };
  constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
  const int presetW = (contentW - (kPresetCount - 1) * 8) / kPresetCount;

  for (int i = 0; i < kPresetCount; i++) {
    lv_obj_t* pBtn = lv_btn_create(presetRow);
    lv_obj_remove_style_all(pBtn);
    lv_obj_set_size(pBtn, presetW, 34);
    lv_obj_set_style_bg_color(pBtn, lv_color_hex(StudioTheme::ELEVATED), 0);
    lv_obj_set_style_bg_color(pBtn, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(pBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(pBtn, lv_color_hex(StudioTheme::RULE), 0);
    lv_obj_set_style_border_width(pBtn, 1, 0);
    lv_obj_set_style_radius(pBtn, 8, 0);
    lv_obj_add_event_cb(pBtn, onPresetClick, LV_EVENT_CLICKED, (void*)(intptr_t)kPresets[i]);

    lv_obj_t* pLbl = lv_label_create(pBtn);
    lv_label_set_text_fmt(pLbl, "%dm", kPresets[i]);
    lv_obj_set_style_text_font(pLbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(pLbl, lv_color_hex(StudioTheme::TEXT), 0);
    lv_obj_center(pLbl);
  }

  refreshTimerUi();
  lv_timer_create(onTimerTick, 100, nullptr);
  return s_pageTimer;
}
