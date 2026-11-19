#include "app_home.h"
#include "../../os/studio_theme.h"
#include "../../utils/scaled_font.h"
#include "../../utils/ui_settings.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static lv_obj_t*   s_pageHome   = nullptr;
static lv_obj_t*   s_lblClock   = nullptr;
static lv_obj_t*   s_lblDay     = nullptr;
static lv_obj_t*   s_lblDate    = nullptr;
static lv_timer_t* s_clockTimer = nullptr;

static void initFallbackRtcFromBuildTime() {
  if (time(nullptr) >= 1700000000L) return;

  const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  char mStr[4] = {0};
  int day = 1, year = 2026, hh = 0, mm = 0, ss = 0;
  sscanf(__DATE__, "%3s %d %d", mStr, &day, &year);
  sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);

  int mon = 0;
  for (int i = 0; i < 12; i++) {
    if (strncmp(mStr, months[i], 3) == 0) {
      mon = i;
      break;
    }
  }

  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon  = mon;
  t.tm_mday = day;
  t.tm_hour = hh;
  t.tm_min  = mm;
  t.tm_sec  = ss;
  time_t epoch = mktime(&t);
  if (epoch > 0) {
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
  }
}

static void updateClockDisplay(lv_timer_t* timer) {
  (void)timer;
  if (!s_lblClock || !s_lblDay || !s_lblDate) return;

  static const char* kDays[] = {
    "Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu"
  };
  static const char* kMonths[] = {
    "Januari", "Februari", "Maret", "April", "Mei", "Juni",
    "Juli", "Agustus", "September", "Oktober", "November", "Desember"
  };

  time_t now = time(nullptr);
  struct tm tmNow = {};
  localtime_r(&now, &tmNow);

  char buf[32];
  if (uiSettingsGet().showSeconds) {
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
  } else {
    snprintf(buf, sizeof(buf), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
  }
  lv_label_set_text(s_lblClock, buf);

  const char* dayName = (tmNow.tm_wday >= 0 && tmNow.tm_wday < 7) ? kDays[tmNow.tm_wday] : "-";
  lv_label_set_text(s_lblDay, dayName);

  const char* monName = (tmNow.tm_mon >= 0 && tmNow.tm_mon < 12) ? kMonths[tmNow.tm_mon] : "-";
  snprintf(buf, sizeof(buf), "%d %s %d", tmNow.tm_mday, monName, tmNow.tm_year + 1900);
  lv_label_set_text(s_lblDate, buf);
}

static void onHomeSettingsChanged(const UiSettings& cfg) {
  if (!s_pageHome) return;
  updateScaledClockFontMetrics();

  lv_obj_set_style_pad_row(s_pageHome, uiScalePx(cfg.rowGapPx), 0);

  if (s_lblClock) {
    lv_obj_set_style_text_font(s_lblClock, getScaledClockFont(), 0);
    lv_obj_refresh_style(s_lblClock, LV_PART_MAIN, LV_STYLE_TEXT_FONT);
  }

  const lv_font_t* dayFont  = uiPickMontserratFont(28);
  const lv_font_t* dateFont = uiPickMontserratFont(24);

  if (s_lblDay) {
    lv_obj_set_style_text_font(s_lblDay, dayFont, 0);
    lv_obj_set_style_pad_top(s_lblDay, uiScalePx(4), 0);
    if (cfg.showDate) lv_obj_clear_flag(s_lblDay, LV_OBJ_FLAG_HIDDEN);
    else              lv_obj_add_flag(s_lblDay, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_lblDate) {
    lv_obj_set_style_text_font(s_lblDate, dateFont, 0);
    if (cfg.showDate) lv_obj_clear_flag(s_lblDate, LV_OBJ_FLAG_HIDDEN);
    else              lv_obj_add_flag(s_lblDate, LV_OBJ_FLAG_HIDDEN);
  }

  updateClockDisplay(nullptr);
  lv_obj_mark_layout_as_dirty(s_pageHome);
  lv_obj_update_layout(s_pageHome);
  lv_obj_invalidate(s_pageHome);
}

static lv_obj_t* homeOnCreate(lv_obj_t* parent, int w, int h) {
  initScaledClockFont();

  s_pageHome = makePlainBox(parent, 0, 0, w, h);
  lv_obj_set_layout(s_pageHome, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_pageHome, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_pageHome, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_pageHome, uiScalePx(uiSettingsGet().rowGapPx), 0);

  s_lblClock = lv_label_create(s_pageHome);
  lv_obj_set_style_text_font(s_lblClock, getScaledClockFont(), 0);
  lv_obj_set_style_text_color(s_lblClock, lv_color_hex(StudioTheme::TEXT), 0);
  lv_obj_set_style_text_letter_space(s_lblClock, 1, 0);

  s_lblDay = lv_label_create(s_pageHome);
  lv_obj_set_style_text_font(s_lblDay, uiPickMontserratFont(28), 0);
  lv_obj_set_style_text_color(s_lblDay, lv_color_hex(StudioTheme::CHAMPAGNE), 0);
  lv_obj_set_style_pad_top(s_lblDay, uiScalePx(4), 0);

  s_lblDate = lv_label_create(s_pageHome);
  lv_obj_set_style_text_font(s_lblDate, uiPickMontserratFont(24), 0);
  lv_obj_set_style_text_color(s_lblDate, lv_color_hex(StudioTheme::MUTED), 0);

  initFallbackRtcFromBuildTime();
  uiSettingsSubscribe(onHomeSettingsChanged);
  onHomeSettingsChanged(uiSettingsGet());
  s_clockTimer = lv_timer_create(updateClockDisplay, 1000, nullptr);

  return s_pageHome;
}

static void homeOnDestroy() {
  if (s_clockTimer) {
    lv_timer_del(s_clockTimer);
    s_clockTimer = nullptr;
  }
  s_pageHome = nullptr;
  s_lblClock = nullptr;
  s_lblDay   = nullptr;
  s_lblDate  = nullptr;
}

static void homeOnResume() {
  updateClockDisplay(nullptr);
}

AppDescriptor appHomeDescriptor() {
  return AppDescriptor{
    "home",
    "Home",
    LV_SYMBOL_HOME,
    nullptr,
    true, // Home tetap hidup sebagai launcher utama
    homeOnCreate,
    homeOnDestroy,
    homeOnResume,
    nullptr,
    nullptr
  };
}
