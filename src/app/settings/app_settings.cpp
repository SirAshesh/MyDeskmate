#include "app_settings.h"
#include "../../os/app_manager.h"
#include "../../os/studio_theme.h"
#include "../../hal/touch_4wire.h"
#include "../../utils/ui_settings.h"

static lv_obj_t* s_sldScale        = nullptr;
static lv_obj_t* s_lblScaleVal     = nullptr;
static lv_obj_t* s_sldGap          = nullptr;
static lv_obj_t* s_lblGapVal       = nullptr;
static lv_obj_t* s_swSeconds       = nullptr;
static lv_obj_t* s_swDate          = nullptr;
static lv_obj_t* s_swShowTouch     = nullptr;
static lv_obj_t* s_swHideTouchLoc  = nullptr;
static lv_obj_t* s_lblSettingsInfo = nullptr;
static lv_obj_t* s_lblGuardStatus  = nullptr;

static void onClearQuarantineClick(lv_event_t* e) {
  (void)e;
  appManagerClearAllQuarantine();
  if (s_lblGuardStatus) {
    lv_label_set_text(s_lblGuardStatus, "Karantina: 0 app (Aman)");
    lv_obj_set_style_text_color(s_lblGuardStatus, lv_color_hex(StudioTheme::SAGE), 0);
  }
}

static void syncSettingsControls(const UiSettings& cfg) {
  if (s_sldScale) lv_slider_set_value(s_sldScale, cfg.scalePct, LV_ANIM_OFF);
  if (s_lblScaleVal) lv_label_set_text_fmt(s_lblScaleVal, "%d%%", cfg.scalePct);

  if (s_sldGap) lv_slider_set_value(s_sldGap, cfg.rowGapPx, LV_ANIM_OFF);
  if (s_lblGapVal) lv_label_set_text_fmt(s_lblGapVal, "%d px", cfg.rowGapPx);

  if (s_swSeconds) {
    if (cfg.showSeconds) lv_obj_add_state(s_swSeconds, LV_STATE_CHECKED);
    else                 lv_obj_clear_state(s_swSeconds, LV_STATE_CHECKED);
  }
  if (s_swDate) {
    if (cfg.showDate) lv_obj_add_state(s_swDate, LV_STATE_CHECKED);
    else              lv_obj_clear_state(s_swDate, LV_STATE_CHECKED);
  }
  if (s_swShowTouch) {
    if (cfg.showTouchDot) lv_obj_add_state(s_swShowTouch, LV_STATE_CHECKED);
    else                  lv_obj_clear_state(s_swShowTouch, LV_STATE_CHECKED);
  }
  if (s_swHideTouchLoc) {
    if (cfg.hideTouchCoords) lv_obj_add_state(s_swHideTouchLoc, LV_STATE_CHECKED);
    else                     lv_obj_clear_state(s_swHideTouchLoc, LV_STATE_CHECKED);
  }
}

static void onScaleSliderChanged(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t* slider = lv_event_get_target(e);
  UiSettings next = uiSettingsGet();
  if (code == LV_EVENT_VALUE_CHANGED) {
    next.scalePct = lv_slider_get_value(slider);
    if (s_lblScaleVal) lv_label_set_text_fmt(s_lblScaleVal, "%d%%", next.scalePct);
    uiSettingsUpdate(next, false);
  } else if (code == LV_EVENT_RELEASED) {
    next.scalePct = lv_slider_get_value(slider);
    uiSettingsUpdate(next, true);
    if (s_lblSettingsInfo) lv_label_set_text(s_lblSettingsInfo, "Tersimpan");
  }
}

static void onGapSliderChanged(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t* slider = lv_event_get_target(e);
  UiSettings next = uiSettingsGet();
  if (code == LV_EVENT_VALUE_CHANGED) {
    next.rowGapPx = lv_slider_get_value(slider);
    if (s_lblGapVal) lv_label_set_text_fmt(s_lblGapVal, "%d px", next.rowGapPx);
    uiSettingsUpdate(next, false);
  } else if (code == LV_EVENT_RELEASED) {
    next.rowGapPx = lv_slider_get_value(slider);
    uiSettingsUpdate(next, true);
    if (s_lblSettingsInfo) lv_label_set_text(s_lblSettingsInfo, "Tersimpan");
  }
}

static void onSecondsSwitchChanged(lv_event_t* e) {
  lv_obj_t* sw = lv_event_get_target(e);
  UiSettings next = uiSettingsGet();
  next.showSeconds = lv_obj_has_state(sw, LV_STATE_CHECKED);
  uiSettingsUpdate(next, true);
  if (s_lblSettingsInfo) lv_label_set_text(s_lblSettingsInfo, "Tersimpan");
}

static void onDateSwitchChanged(lv_event_t* e) {
  lv_obj_t* sw = lv_event_get_target(e);
  UiSettings next = uiSettingsGet();
  next.showDate = lv_obj_has_state(sw, LV_STATE_CHECKED);
  uiSettingsUpdate(next, true);
  if (s_lblSettingsInfo) lv_label_set_text(s_lblSettingsInfo, "Tersimpan");
}

static void onShowTouchSwitchChanged(lv_event_t* e) {
  lv_obj_t* sw = lv_event_get_target(e);
  UiSettings next = uiSettingsGet();
  next.showTouchDot = lv_obj_has_state(sw, LV_STATE_CHECKED);
  uiSettingsUpdate(next, true);
  if (s_lblSettingsInfo) lv_label_set_text(s_lblSettingsInfo, "Tersimpan");
}

static void onHideTouchLocSwitchChanged(lv_event_t* e) {
  lv_obj_t* sw = lv_event_get_target(e);
  UiSettings next = uiSettingsGet();
  next.hideTouchCoords = lv_obj_has_state(sw, LV_STATE_CHECKED);
  uiSettingsUpdate(next, true);
  if (s_lblSettingsInfo) lv_label_set_text(s_lblSettingsInfo, "Tersimpan");
}

static void onResetSettingsClick(lv_event_t* e) {
  (void)e;
  uiSettingsReset();
  syncSettingsControls(uiSettingsGet());
  if (s_lblSettingsInfo) lv_label_set_text(s_lblSettingsInfo, "Reset ke default");
}

static void onCalibrateTouchClick(lv_event_t* e) {
  (void)e;
  runTouchCalibration();
  if (s_lblSettingsInfo) {
    char calBuf[64];
    getTouchCalString(calBuf, sizeof(calBuf));
    lv_label_set_text(s_lblSettingsInfo, calBuf);
  }
}

static lv_obj_t* makeSettingsCard(lv_obj_t* parent, int w) {
  lv_obj_t* card = lv_obj_create(parent);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, w, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(card, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_pad_hor(card, 14, 0);
  lv_obj_set_style_pad_ver(card, 10, 0);
  lv_obj_set_style_pad_row(card, 8, 0);
  lv_obj_set_layout(card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  return card;
}

static void styleSettingsSlider(lv_obj_t* slider, uint32_t accentHex) {
  lv_obj_set_size(slider, lv_pct(100), 8);
  lv_obj_set_style_bg_color(slider, lv_color_hex(StudioTheme::TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);

  lv_obj_set_style_bg_color(slider, lv_color_hex(accentHex), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);

  lv_obj_set_style_bg_color(slider, lv_color_hex(StudioTheme::TEXT), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_border_color(slider, lv_color_hex(accentHex), LV_PART_KNOB);
  lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 5, LV_PART_KNOB);
}

static void styleSettingsSwitch(lv_obj_t* sw, uint32_t accentHex) {
  lv_obj_set_size(sw, 44, 24);
  lv_obj_set_style_bg_color(sw, lv_color_hex(StudioTheme::TRACK), LV_PART_MAIN);
  lv_obj_set_style_border_color(sw, lv_color_hex(StudioTheme::RULE), LV_PART_MAIN);
  lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);

  lv_obj_set_style_bg_color(sw, lv_color_hex(accentHex), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, lv_color_hex(StudioTheme::ELEVATED), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sw, lv_color_hex(StudioTheme::TEXT), LV_PART_KNOB);
}

static lv_obj_t* settingsOnCreate(lv_obj_t* parent, int rightW, int rightH) {
  lv_obj_t* pageSettings = makePlainBox(parent, 0, 0, rightW, rightH);
  lv_obj_add_flag(pageSettings, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(pageSettings, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(pageSettings, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_pad_hor(pageSettings, 16, 0);
  lv_obj_set_style_pad_ver(pageSettings, 12, 0);
  lv_obj_set_style_pad_row(pageSettings, 10, 0);
  lv_obj_set_layout(pageSettings, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(pageSettings, LV_FLEX_FLOW_COLUMN);

  const int cardW = rightW - 32;

  lv_obj_t* hdr = makePlainBox(pageSettings, 0, 0, cardW, LV_SIZE_CONTENT);
  lv_obj_set_layout(hdr, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* lblTitle = lv_label_create(hdr);
  lv_label_set_text(lblTitle, "Settings");
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblTitle, lv_color_hex(StudioTheme::TEXT), 0);

  lv_obj_t* lblSub = lv_label_create(hdr);
  lv_label_set_text(lblSub, "Skala Konten Kanan");
  lv_obj_set_style_text_font(lblSub, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblSub, lv_color_hex(StudioTheme::MUTED), 0);

  // Card 1: Slider Skala Konten
  lv_obj_t* cardScale = makeSettingsCard(pageSettings, cardW);
  lv_obj_t* rowScaleHdr = makePlainBox(cardScale, 0, 0, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_layout(rowScaleHdr, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(rowScaleHdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rowScaleHdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* lblScaleTitle = lv_label_create(rowScaleHdr);
  lv_label_set_text(lblScaleTitle, "Skala Konten");
  lv_obj_set_style_text_font(lblScaleTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblScaleTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_lblScaleVal = lv_label_create(rowScaleHdr);
  lv_obj_set_style_text_font(s_lblScaleVal, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lblScaleVal, lv_color_hex(StudioTheme::CHAMPAGNE), 0);

  s_sldScale = lv_slider_create(cardScale);
  lv_slider_set_range(s_sldScale, 60, 110);
  styleSettingsSlider(s_sldScale, StudioTheme::CHAMPAGNE);
  lv_obj_add_event_cb(s_sldScale, onScaleSliderChanged, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_sldScale, onScaleSliderChanged, LV_EVENT_RELEASED, nullptr);

  // Card 2: Slider Jarak Baris
  lv_obj_t* cardGap = makeSettingsCard(pageSettings, cardW);
  lv_obj_t* rowGapHdr = makePlainBox(cardGap, 0, 0, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_layout(rowGapHdr, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(rowGapHdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rowGapHdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* lblGapTitle = lv_label_create(rowGapHdr);
  lv_label_set_text(lblGapTitle, "Jarak Baris");
  lv_obj_set_style_text_font(lblGapTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblGapTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_lblGapVal = lv_label_create(rowGapHdr);
  lv_obj_set_style_text_font(s_lblGapVal, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lblGapVal, lv_color_hex(StudioTheme::TEAL), 0);

  s_sldGap = lv_slider_create(cardGap);
  lv_slider_set_range(s_sldGap, 2, 24);
  styleSettingsSlider(s_sldGap, StudioTheme::TEAL);
  lv_obj_add_event_cb(s_sldGap, onGapSliderChanged, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_sldGap, onGapSliderChanged, LV_EVENT_RELEASED, nullptr);

  // Card 3: Switch Detik & Tanggal
  lv_obj_t* cardToggles = makeSettingsCard(pageSettings, cardW);
  lv_obj_set_flex_flow(cardToggles, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(cardToggles, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* grpSec = makePlainBox(cardToggles, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_layout(grpSec, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(grpSec, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(grpSec, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(grpSec, 10, 0);

  lv_obj_t* lblSec = lv_label_create(grpSec);
  lv_label_set_text(lblSec, "Detik");
  lv_obj_set_style_text_font(lblSec, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblSec, lv_color_hex(StudioTheme::TEXT), 0);

  s_swSeconds = lv_switch_create(grpSec);
  styleSettingsSwitch(s_swSeconds, StudioTheme::CHAMPAGNE);
  lv_obj_add_event_cb(s_swSeconds, onSecondsSwitchChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t* grpDate = makePlainBox(cardToggles, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_layout(grpDate, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(grpDate, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(grpDate, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(grpDate, 10, 0);

  lv_obj_t* lblDt = lv_label_create(grpDate);
  lv_label_set_text(lblDt, "Tanggal");
  lv_obj_set_style_text_font(lblDt, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblDt, lv_color_hex(StudioTheme::TEXT), 0);

  s_swDate = lv_switch_create(grpDate);
  styleSettingsSwitch(s_swDate, StudioTheme::TEAL);
  lv_obj_add_event_cb(s_swDate, onDateSwitchChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  // Card 4: Switch Debug Sentuhan
  lv_obj_t* cardTouchDbg = makeSettingsCard(pageSettings, cardW);
  lv_obj_set_style_pad_row(cardTouchDbg, 8, 0);

  lv_obj_t* rowShowTouch = makePlainBox(cardTouchDbg, 0, 0, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_layout(rowShowTouch, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(rowShowTouch, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rowShowTouch, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* lblShowTouch = lv_label_create(rowShowTouch);
  lv_label_set_text(lblShowTouch, "Tampilkan Sentuhan");
  lv_obj_set_style_text_font(lblShowTouch, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblShowTouch, lv_color_hex(StudioTheme::TEXT), 0);

  s_swShowTouch = lv_switch_create(rowShowTouch);
  styleSettingsSwitch(s_swShowTouch, StudioTheme::CORAL);
  lv_obj_add_event_cb(s_swShowTouch, onShowTouchSwitchChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t* rowHideLoc = makePlainBox(cardTouchDbg, 0, 0, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_layout(rowHideLoc, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(rowHideLoc, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rowHideLoc, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* lblHideLoc = lv_label_create(rowHideLoc);
  lv_label_set_text(lblHideLoc, "Sembunyikan Lokasi Sentuhan");
  lv_obj_set_style_text_font(lblHideLoc, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblHideLoc, lv_color_hex(StudioTheme::TEXT), 0);

  s_swHideTouchLoc = lv_switch_create(rowHideLoc);
  styleSettingsSwitch(s_swHideTouchLoc, StudioTheme::SAGE);
  lv_obj_add_event_cb(s_swHideTouchLoc, onHideTouchLocSwitchChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  // Footer
  lv_obj_t* ftr = makePlainBox(pageSettings, 0, 0, cardW, LV_SIZE_CONTENT);
  lv_obj_set_layout(ftr, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ftr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ftr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  s_lblSettingsInfo = lv_label_create(ftr);
  {
    char calBuf[64];
    getTouchCalString(calBuf, sizeof(calBuf));
    lv_label_set_text(s_lblSettingsInfo, calBuf);
  }
  lv_obj_set_style_text_font(s_lblSettingsInfo, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblSettingsInfo, lv_color_hex(StudioTheme::MUTED), 0);

  lv_obj_t* ftrBtns = makePlainBox(ftr, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_layout(ftrBtns, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ftrBtns, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(ftrBtns, 8, 0);

  lv_obj_t* btnCal = lv_btn_create(ftrBtns);
  lv_obj_remove_style_all(btnCal);
  lv_obj_set_style_bg_color(btnCal, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(btnCal, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btnCal, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btnCal, lv_color_hex(StudioTheme::TEAL), 0);
  lv_obj_set_style_border_width(btnCal, 1, 0);
  lv_obj_set_style_radius(btnCal, 8, 0);
  lv_obj_set_style_pad_hor(btnCal, 12, 0);
  lv_obj_set_style_pad_ver(btnCal, 7, 0);
  lv_obj_add_event_cb(btnCal, onCalibrateTouchClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lblCal = lv_label_create(btnCal);
  lv_label_set_text(lblCal, "Kalibrasi");
  lv_obj_set_style_text_font(lblCal, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblCal, lv_color_hex(StudioTheme::TEAL), 0);
  lv_obj_center(lblCal);

  lv_obj_t* btnReset = lv_btn_create(ftrBtns);
  lv_obj_remove_style_all(btnReset);
  lv_obj_set_style_bg_color(btnReset, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(btnReset, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btnReset, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btnReset, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(btnReset, 1, 0);
  lv_obj_set_style_radius(btnReset, 8, 0);
  lv_obj_set_style_pad_hor(btnReset, 12, 0);
  lv_obj_set_style_pad_ver(btnReset, 7, 0);
  lv_obj_add_event_cb(btnReset, onResetSettingsClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lblReset = lv_label_create(btnReset);
  lv_label_set_text(lblReset, "Reset Default");
  lv_obj_set_style_text_font(lblReset, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblReset, lv_color_hex(StudioTheme::CHAMPAGNE), 0);
  lv_obj_center(lblReset);

  // Card 5: Crash Guard (B)
  lv_obj_t* cardGuard = makeSettingsCard(pageSettings, cardW);
  lv_obj_t* rowGuard  = makePlainBox(cardGuard, 0, 0, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_layout(rowGuard, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(rowGuard, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rowGuard, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  s_lblGuardStatus = lv_label_create(rowGuard);
  int qCount = appManagerGetQuarantineCount();
  if (qCount > 0) {
    lv_label_set_text_fmt(s_lblGuardStatus, "Karantina: %d app (%s)", qCount, appManagerGetLastCrashName());
    lv_obj_set_style_text_color(s_lblGuardStatus, lv_color_hex(StudioTheme::ROSE), 0);
  } else {
    lv_label_set_text(s_lblGuardStatus, "Crash Guard: 0 karantina");
    lv_obj_set_style_text_color(s_lblGuardStatus, lv_color_hex(StudioTheme::SAGE), 0);
  }
  lv_obj_set_style_text_font(s_lblGuardStatus, &lv_font_montserrat_12, 0);

  lv_obj_t* btnClearGuard = lv_btn_create(rowGuard);
  lv_obj_remove_style_all(btnClearGuard);
  lv_obj_set_style_bg_color(btnClearGuard, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(btnClearGuard, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btnClearGuard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btnClearGuard, lv_color_hex(StudioTheme::TEAL), 0);
  lv_obj_set_style_border_width(btnClearGuard, 1, 0);
  lv_obj_set_style_radius(btnClearGuard, 8, 0);
  lv_obj_set_style_pad_hor(btnClearGuard, 10, 0);
  lv_obj_set_style_pad_ver(btnClearGuard, 5, 0);
  lv_obj_add_event_cb(btnClearGuard, onClearQuarantineClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lblClearGuard = lv_label_create(btnClearGuard);
  lv_label_set_text(lblClearGuard, "Pulihkan");
  lv_obj_set_style_text_font(lblClearGuard, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblClearGuard, lv_color_hex(StudioTheme::TEAL), 0);
  lv_obj_center(lblClearGuard);

  syncSettingsControls(uiSettingsGet());
  return pageSettings;
}

static void settingsOnDestroy() {
  s_sldScale        = nullptr;
  s_lblScaleVal     = nullptr;
  s_sldGap          = nullptr;
  s_lblGapVal       = nullptr;
  s_swSeconds       = nullptr;
  s_swDate          = nullptr;
  s_swShowTouch     = nullptr;
  s_swHideTouchLoc  = nullptr;
  s_lblSettingsInfo = nullptr;
  s_lblGuardStatus  = nullptr;
}

AppDescriptor appSettingsDescriptor() {
  return AppDescriptor{
    "settings",
    "Settings",
    LV_SYMBOL_SETTINGS,
    nullptr,
    false,
    settingsOnCreate,
    settingsOnDestroy,
    nullptr,
    nullptr,
    nullptr
  };
}
