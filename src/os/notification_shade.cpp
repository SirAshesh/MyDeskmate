#include "notification_shade.h"
#include "app_manager.h"
#include "boot_screen.h"
#include "idle_screen.h"
#include "pc_mouse.h"
#include "../hal/display_hx8357d.h"
#include "../utils/mqtt_notify.h"
#include "../utils/text_sanitize.h"
#include "../utils/ui_settings.h"
#include "ui_config.h"
#include "wifi_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {
constexpr int MAX_NOTIFICATIONS = 12;
constexpr int SWIPE_START_TOP_MAX_Y = 68;
constexpr int SWIPE_OPEN_THRESHOLD_Y = 44;
constexpr int SWIPE_CLOSE_THRESHOLD_Y = 44;
constexpr int SHADE_ANIM_MS = 240;

constexpr int HEADER_H = 44;
constexpr int BOTTOM_BAR_H = 26;
constexpr int BODY_H = UI_H - HEADER_H - BOTTOM_BAR_H; // 250 px

enum class ShadeMode : uint8_t {
  NOTIF   = 0,
  WIDGETS = 1,
};

struct NotificationItem {
  char     title[48];
  char     body[128];
  char     timeStr[16];
  char     key[96];
  uint32_t accentHex;
};

struct QuickTileUi {
  lv_obj_t* btn;
  lv_obj_t* circle;
  lv_obj_t* icon;
  lv_obj_t* title;
  lv_obj_t* sub;
  uint32_t  accentHex;
};

NotificationItem s_items[MAX_NOTIFICATIONS];
int s_count = 0;

lv_obj_t* s_topHintPill   = nullptr;
lv_obj_t* s_shade         = nullptr;

// Header widgets
lv_obj_t* s_hdrIcon       = nullptr;
lv_obj_t* s_hdrTitle      = nullptr;
lv_obj_t* s_hdrBadge      = nullptr;
lv_obj_t* s_tabNotifBtn   = nullptr;
lv_obj_t* s_tabNotifLbl   = nullptr;
lv_obj_t* s_tabWidgetBtn  = nullptr;
lv_obj_t* s_tabWidgetLbl  = nullptr;
lv_obj_t* s_clearBtn      = nullptr;

// Notifikasi view
lv_obj_t* s_list          = nullptr;
lv_obj_t* s_emptyBox      = nullptr;

// Android Quick Widget view
lv_obj_t* s_widgetView    = nullptr;
QuickTileUi s_tileWifi    = {};
QuickTileUi s_tileNet     = {};
QuickTileUi s_tileMouse   = {};
QuickTileUi s_tileTouch   = {};
QuickTileUi s_tileMqtt    = {};
QuickTileUi s_tileSleep   = {};
lv_obj_t* s_sldScale      = nullptr;
lv_obj_t* s_lblScaleVal   = nullptr;

lv_timer_t* s_widgetRefreshTimer = nullptr;

ShadeMode s_mode          = ShadeMode::NOTIF;
bool      s_isOpen        = false;
bool      s_animating     = false;

// Gesture tracking via pointer indev
bool    s_tracking        = false;
bool    s_dragging        = false;
int16_t s_startX          = 0;
int16_t s_startY          = 0;
int16_t s_lastDy          = 0;
} // namespace

static void animYCb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, v);
}

static void showHintPills(bool visible) {
  if (s_topHintPill) {
    if (visible) lv_obj_clear_flag(s_topHintPill, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_topHintPill, LV_OBJ_FLAG_HIDDEN);
  }
}

static void onOpenAnimReady(lv_anim_t* /*a*/) {
  s_animating = false;
}

static void onCloseAnimReady(lv_anim_t* /*a*/) {
  s_animating = false;
  if (s_shade) {
    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
  }
  showHintPills(true);
}

static void openAppById(const char* appId) {
  if (!appId) return;
  int cnt = appManagerGetCount();
  for (int i = 0; i < cnt; i++) {
    const AppDescriptor* desc = appManagerGetDescriptor(i);
    if (desc && desc->id && strcmp(desc->id, appId) == 0) {
      notificationShadeClose();
      appManagerOpen(i);
      return;
    }
  }
}

static void updateTileAppearance(QuickTileUi& tile, bool active, const char* subText) {
  if (!tile.btn || !tile.circle || !tile.icon || !tile.sub) return;

  if (active) {
    lv_obj_set_style_bg_color(tile.btn, lv_color_hex(StudioTheme::ELEVATED), 0);
    lv_obj_set_style_border_color(tile.btn, lv_color_hex(tile.accentHex), 0);
    lv_obj_set_style_bg_color(tile.circle, lv_color_hex(tile.accentHex), 0);
    lv_obj_set_style_text_color(tile.icon, lv_color_hex(StudioTheme::BG), 0);
    lv_obj_set_style_text_color(tile.sub, lv_color_hex(tile.accentHex), 0);
  } else {
    lv_obj_set_style_bg_color(tile.btn, lv_color_hex(StudioTheme::SURFACE), 0);
    lv_obj_set_style_border_color(tile.btn, lv_color_hex(StudioTheme::RULE), 0);
    lv_obj_set_style_bg_color(tile.circle, lv_color_hex(StudioTheme::TRACK), 0);
    lv_obj_set_style_text_color(tile.icon, lv_color_hex(StudioTheme::MUTED), 0);
    lv_obj_set_style_text_color(tile.sub, lv_color_hex(StudioTheme::MUTED), 0);
  }

  if (subText) {
    lv_label_set_text(tile.sub, subText);
  }
}

static void refreshWidgetsState() {
  if (!s_widgetView) return;

  // Header status jam + free internal RAM saat tab Widget aktif
  if (s_mode == ShadeMode::WIDGETS && s_hdrBadge) {
    time_t now = time(nullptr);
    struct tm tmNow = {};
    localtime_r(&now, &tmNow);
    char hdrInfo[32];
    if (now >= 1700000000L) {
      snprintf(hdrInfo, sizeof(hdrInfo), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
    } else {
      snprintf(hdrInfo, sizeof(hdrInfo), "%u KB", (unsigned)(ESP.getFreeHeap() / 1024));
    }
    lv_label_set_text(s_hdrBadge, hdrInfo);
  }

  // 1. Wi-Fi
  wifi_mode_t wMode = WiFi.getMode();
  wl_status_t wSt   = WiFi.status();
  bool wifiOn       = (wMode != WIFI_OFF);
  char wifiSub[32];
  if (!wifiOn) {
    snprintf(wifiSub, sizeof(wifiSub), "Mati");
  } else if (wSt == WL_CONNECTED) {
    snprintf(wifiSub, sizeof(wifiSub), "%s (%ddBm)", WIFI_SSID, WiFi.RSSI());
  } else {
    snprintf(wifiSub, sizeof(wifiSub), "Menyambung...");
  }
  updateTileAppearance(s_tileWifi, (wSt == WL_CONNECTED) || wifiOn, wifiSub);

  // 2. IP & Sync Net/NTP
  if (wSt == WL_CONNECTED) {
    char ipBuf[24];
    snprintf(ipBuf, sizeof(ipBuf), "%s", WiFi.localIP().toString().c_str());
    updateTileAppearance(s_tileNet, true, ipBuf);
  } else {
    updateTileAppearance(s_tileNet, false, "Offline");
  }

  // 3. PC Mouse UDP Bridge
  bool mouseEn  = pcMouseIsEnabled();
  bool mouseAct = pcMouseIsActive();
  const char* mouseSub = !mouseEn ? "Terkunci" : (mouseAct ? "Terhubung" : "Siap :8789");
  updateTileAppearance(s_tileMouse, mouseEn, mouseSub);

  // 4. Titik Sentuh Debug
  const UiSettings& cfg = uiSettingsGet();
  updateTileAppearance(s_tileTouch, cfg.showTouchDot, cfg.showTouchDot ? "Aktif" : "Sembunyi");

  // 5. MQTT / Bridge Status
  char mqttBuf[40] = "";
  mqttNotifyGetStatus(mqttBuf, sizeof(mqttBuf));
  bool mqttOk = (strstr(mqttBuf, "OK") != nullptr || strstr(mqttBuf, "Connected") != nullptr);
  updateTileAppearance(s_tileMqtt, mqttOk, mqttBuf[0] ? mqttBuf : "System Info");

  // 6. Layar Tidur (Idle Screen)
  updateTileAppearance(s_tileSleep, idleScreenIsActive(), "Jam & Pet");

  // Slider Skala UI
  if (s_sldScale && !lv_obj_has_state(s_sldScale, LV_STATE_PRESSED)) {
    lv_slider_set_value(s_sldScale, cfg.scalePct, LV_ANIM_OFF);
  }
  if (s_lblScaleVal) {
    lv_label_set_text_fmt(s_lblScaleVal, "%d%%", cfg.scalePct);
  }
}

static void applyShadeMode(ShadeMode nextMode) {
  s_mode = nextMode;

  bool isNotif = (nextMode == ShadeMode::NOTIF);

  if (s_list) {
    if (isNotif) lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_widgetView) {
    if (isNotif) lv_obj_add_flag(s_widgetView, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_clear_flag(s_widgetView, LV_OBJ_FLAG_HIDDEN);
  }

  if (s_hdrIcon) {
    lv_label_set_text(s_hdrIcon, isNotif ? LV_SYMBOL_BELL : LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(
      s_hdrIcon,
      lv_color_hex(isNotif ? StudioTheme::CHAMPAGNE : StudioTheme::TEAL),
      0
    );
  }
  if (s_hdrTitle) {
    lv_label_set_text(s_hdrTitle, isNotif ? "Notifikasi" : "Quick Widget");
  }
  if (s_hdrBadge) {
    if (isNotif) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", s_count);
      lv_label_set_text(s_hdrBadge, buf);
    }
  }

  if (s_clearBtn) {
    if (isNotif && s_count > 0) lv_obj_clear_flag(s_clearBtn, LV_OBJ_FLAG_HIDDEN);
    else                        lv_obj_add_flag(s_clearBtn, LV_OBJ_FLAG_HIDDEN);
  }

  if (s_tabNotifBtn && s_tabNotifLbl && s_tabWidgetBtn && s_tabWidgetLbl) {
    lv_obj_set_style_bg_color(
      s_tabNotifBtn,
      lv_color_hex(isNotif ? StudioTheme::ELEVATED : StudioTheme::TRACK),
      0
    );
    lv_obj_set_style_text_color(
      s_tabNotifLbl,
      lv_color_hex(isNotif ? StudioTheme::CHAMPAGNE : StudioTheme::MUTED),
      0
    );

    lv_obj_set_style_bg_color(
      s_tabWidgetBtn,
      lv_color_hex(!isNotif ? StudioTheme::ELEVATED : StudioTheme::TRACK),
      0
    );
    lv_obj_set_style_text_color(
      s_tabWidgetLbl,
      lv_color_hex(!isNotif ? StudioTheme::TEAL : StudioTheme::MUTED),
      0
    );
  }

  if (!isNotif) {
    refreshWidgetsState();
  }
}

static void onDismissCardClick(lv_event_t* e);

static void rebuildNotificationList() {
  if (!s_list || !s_emptyBox) return;

  uint32_t childCnt = lv_obj_get_child_cnt(s_list);
  for (int i = (int)childCnt - 1; i >= 0; i--) {
    lv_obj_t* child = lv_obj_get_child(s_list, i);
    if (child != s_emptyBox) {
      lv_obj_del(child);
    }
  }

  if (s_hdrBadge && s_mode == ShadeMode::NOTIF) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_count);
    lv_label_set_text(s_hdrBadge, buf);
  }

  if (s_clearBtn) {
    if (s_mode == ShadeMode::NOTIF && s_count > 0) lv_obj_clear_flag(s_clearBtn, LV_OBJ_FLAG_HIDDEN);
    else                                           lv_obj_add_flag(s_clearBtn, LV_OBJ_FLAG_HIDDEN);
  }

  if (s_topHintPill) {
    lv_obj_set_style_bg_color(
      s_topHintPill,
      lv_color_hex(s_count > 0 ? StudioTheme::CHAMPAGNE : StudioTheme::SUBTLE),
      0
    );
  }

  if (s_count == 0) {
    lv_obj_clear_flag(s_emptyBox, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  lv_obj_add_flag(s_emptyBox, LV_OBJ_FLAG_HIDDEN);

  for (int i = s_count - 1; i >= 0; i--) {
    const NotificationItem& item = s_items[i];

    lv_obj_t* card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(StudioTheme::ELEVATED), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(StudioTheme::RULE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_left(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* accentBar = makePlainBox(card, -6, 2, 3, 28);
    lv_obj_set_style_bg_color(accentBar, lv_color_hex(item.accentHex), 0);
    lv_obj_set_style_bg_opa(accentBar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accentBar, 2, 0);

    lv_obj_t* topRow = makePlainBox(card, 0, 0, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(topRow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(topRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* lblTitle = lv_label_create(topRow);
    lv_label_set_text(lblTitle, item.title);
    lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lblTitle, lv_color_hex(StudioTheme::TEXT), 0);
    lv_obj_set_flex_grow(lblTitle, 1);
    lv_label_set_long_mode(lblTitle, LV_LABEL_LONG_DOT);

    if (item.timeStr[0] != '\0') {
      lv_obj_t* lblTime = lv_label_create(topRow);
      lv_label_set_text(lblTime, item.timeStr);
      lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(lblTime, lv_color_hex(StudioTheme::SUBTLE), 0);
      lv_obj_set_style_pad_right(lblTime, 6, 0);
    }

    lv_obj_t* btnDismiss = lv_btn_create(topRow);
    lv_obj_remove_style_all(btnDismiss);
    lv_obj_set_size(btnDismiss, 22, 22);
    lv_obj_set_style_bg_color(btnDismiss, lv_color_hex(StudioTheme::SURFACE), 0);
    lv_obj_set_style_bg_color(btnDismiss, lv_color_hex(StudioTheme::RULE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btnDismiss, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btnDismiss, 6, 0);
    lv_obj_add_event_cb(btnDismiss, onDismissCardClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_obj_t* dismissLbl = lv_label_create(btnDismiss);
    lv_label_set_text(dismissLbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(dismissLbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(dismissLbl, lv_color_hex(StudioTheme::MUTED), 0);
    lv_obj_center(dismissLbl);

    if (item.body[0] != '\0') {
      lv_obj_t* lblBody = lv_label_create(card);
      lv_label_set_text(lblBody, item.body);
      lv_obj_set_pos(lblBody, 0, 22);
      lv_obj_set_width(lblBody, LV_PCT(100));
      lv_label_set_long_mode(lblBody, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_font(lblBody, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(lblBody, lv_color_hex(StudioTheme::MUTED), 0);
    }
  }
}

static void onDismissCardClick(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= s_count) return;
  if (s_items[idx].key[0] != '\0') {
    mqttNotifySendDismiss(s_items[idx].key);
  }
  for (int i = idx + 1; i < s_count; i++) {
    s_items[i - 1] = s_items[i];
  }
  s_count--;
  rebuildNotificationList();
}

static void onClearAllClick(lv_event_t* /*e*/) {
  notificationClearAll(true);
}

static void onCloseClick(lv_event_t* /*e*/) {
  notificationShadeClose();
}

static void onTabNotifClick(lv_event_t* /*e*/) {
  applyShadeMode(ShadeMode::NOTIF);
}

static void onTabWidgetClick(lv_event_t* /*e*/) {
  applyShadeMode(ShadeMode::WIDGETS);
}

static void onWifiTileClick(lv_event_t* /*e*/) {
  if (WiFi.getMode() == WIFI_OFF) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
  refreshWidgetsState();
}

static void onNetSyncTileClick(lv_event_t* /*e*/) {
  WiFi.persistent(false);
  WiFi.setSleep(false);
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  } else {
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov", "id.pool.ntp.org");
  }
  refreshWidgetsState();
}

static void onMouseTileClick(lv_event_t* /*e*/) {
  pcMouseSetEnabled(!pcMouseIsEnabled());
  refreshWidgetsState();
}

static void onTouchDotTileClick(lv_event_t* /*e*/) {
  UiSettings next = uiSettingsGet();
  next.showTouchDot = !next.showTouchDot;
  uiSettingsUpdate(next, true);
  refreshWidgetsState();
}

static void onMqttTileClick(lv_event_t* /*e*/) {
  openAppById("system");
}

static void onSleepTileClick(lv_event_t* /*e*/) {
  notificationShadeClose();
  idleScreenEnter();
}

static void onOpenDockerClick(lv_event_t* /*e*/) {
  openAppById("docker");
}

static void onOpenSettingsClick(lv_event_t* /*e*/) {
  openAppById("settings");
}

static void onScaleSliderEvent(lv_event_t* e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t* slider = lv_event_get_target(e);
  if (!slider) return;
  UiSettings next = uiSettingsGet();
  next.scalePct = lv_slider_get_value(slider);
  if (code == LV_EVENT_VALUE_CHANGED) {
    if (s_lblScaleVal) lv_label_set_text_fmt(s_lblScaleVal, "%d%%", next.scalePct);
    uiSettingsUpdate(next, false);
  } else if (code == LV_EVENT_RELEASED) {
    if (s_lblScaleVal) lv_label_set_text_fmt(s_lblScaleVal, "%d%%", next.scalePct);
    uiSettingsUpdate(next, true);
  }
}

static void widgetRefreshTimerCb(lv_timer_t* /*t*/) {
  if (s_isOpen && s_mode == ShadeMode::WIDGETS) {
    refreshWidgetsState();
  }
}

static QuickTileUi createQuickTile(lv_obj_t* parent, int x, int y, int w, int h,
                                 const char* symbol, const char* titleText,
                                 uint32_t accentHex, lv_event_cb_t clickCb) {
  QuickTileUi t = {};
  t.accentHex = accentHex;

  t.btn = lv_btn_create(parent);
  lv_obj_remove_style_all(t.btn);
  lv_obj_set_pos(t.btn, x, y);
  lv_obj_set_size(t.btn, w, h);
  lv_obj_set_style_bg_color(t.btn, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_opa(t.btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(t.btn, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(t.btn, 1, 0);
  lv_obj_set_style_radius(t.btn, 12, 0);
  lv_obj_clear_flag(t.btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(t.btn, clickCb, LV_EVENT_CLICKED, nullptr);

  t.circle = makePlainBox(t.btn, 10, (h - 34) / 2, 34, 34);
  lv_obj_set_style_bg_color(t.circle, lv_color_hex(StudioTheme::TRACK), 0);
  lv_obj_set_style_bg_opa(t.circle, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(t.circle, LV_RADIUS_CIRCLE, 0);

  t.icon = lv_label_create(t.circle);
  lv_label_set_text(t.icon, symbol);
  lv_obj_set_style_text_font(t.icon, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(t.icon, lv_color_hex(StudioTheme::MUTED), 0);
  lv_obj_center(t.icon);

  t.title = lv_label_create(t.btn);
  lv_label_set_text(t.title, titleText);
  lv_obj_set_pos(t.title, 52, 14);
  lv_obj_set_width(t.title, w - 58);
  lv_label_set_long_mode(t.title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(t.title, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(t.title, lv_color_hex(StudioTheme::TEXT), 0);

  t.sub = lv_label_create(t.btn);
  lv_label_set_text(t.sub, "-");
  lv_obj_set_pos(t.sub, 52, 36);
  lv_obj_set_width(t.sub, w - 58);
  lv_label_set_long_mode(t.sub, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(t.sub, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(t.sub, lv_color_hex(StudioTheme::MUTED), 0);

  return t;
}

static lv_obj_t* createShortcutTile(lv_obj_t* parent, int x, int y, int w, int h,
                                    const char* symbol, const char* labelText,
                                    uint32_t accentHex, lv_event_cb_t clickCb) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::ELEVATED), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(btn, clickCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* ic = lv_label_create(btn);
  lv_label_set_text(ic, symbol);
  lv_obj_set_style_text_font(ic, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(ic, lv_color_hex(accentHex), 0);
  lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 12);

  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, labelText);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(StudioTheme::TEXT), 0);
  lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -10);

  return btn;
}

static void buildWidgetView() {
  s_widgetView = makePlainBox(s_shade, 0, HEADER_H, UI_W, BODY_H);
  lv_obj_set_style_bg_color(s_widgetView, lv_color_hex(StudioTheme::BG), 0);
  lv_obj_set_style_bg_opa(s_widgetView, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_widgetView, LV_OBJ_FLAG_HIDDEN);

  constexpr int PAD_X = 12;
  constexpr int PAD_Y = 10;
  constexpr int GAP   = 10;
  constexpr int TILE_W = (UI_W - 2 * PAD_X - 2 * GAP) / 3; // 145
  constexpr int TILE_H = 68;

  // Baris 1: Wi-Fi, Sync Net/NTP, PC Mouse
  s_tileWifi = createQuickTile(
    s_widgetView, PAD_X, PAD_Y, TILE_W, TILE_H,
    LV_SYMBOL_WIFI, "Wi-Fi", StudioTheme::TEAL, onWifiTileClick
  );
  s_tileNet = createQuickTile(
    s_widgetView, PAD_X + TILE_W + GAP, PAD_Y, TILE_W, TILE_H,
    LV_SYMBOL_LOOP, "Net & NTP", StudioTheme::TEAL, onNetSyncTileClick
  );
  s_tileMouse = createQuickTile(
    s_widgetView, PAD_X + 2 * (TILE_W + GAP), PAD_Y, TILE_W, TILE_H,
    LV_SYMBOL_NEXT, "PC Mouse", StudioTheme::SAGE, onMouseTileClick
  );

  // Baris 2: Titik Sentuh, MQTT/System, Layar Tidur
  const int row2Y = PAD_Y + TILE_H + GAP;
  s_tileTouch = createQuickTile(
    s_widgetView, PAD_X, row2Y, TILE_W, TILE_H,
    LV_SYMBOL_EDIT, "Touch Dot", StudioTheme::CHAMPAGNE, onTouchDotTileClick
  );
  s_tileMqtt = createQuickTile(
    s_widgetView, PAD_X + TILE_W + GAP, row2Y, TILE_W, TILE_H,
    LV_SYMBOL_REFRESH, "MQTT Sync", StudioTheme::SAGE, onMqttTileClick
  );
  s_tileSleep = createQuickTile(
    s_widgetView, PAD_X + 2 * (TILE_W + GAP), row2Y, TILE_W, TILE_H,
    LV_SYMBOL_POWER, "Sleep Mode", StudioTheme::CORAL, onSleepTileClick
  );

  // Baris 3: Kartu Slider Skala UI (ala Brightness Android) + 2 Shortcut Aplikasi
  const int row3Y = row2Y + TILE_H + GAP;
  constexpr int SLIDER_CARD_W = 274;
  constexpr int SHORTCUT_AREA_X = PAD_X + SLIDER_CARD_W + GAP; // 296
  constexpr int SHORTCUT_W = (UI_W - PAD_X - SHORTCUT_AREA_X - GAP) / 2; // 81

  lv_obj_t* sliderCard = makePlainBox(s_widgetView, PAD_X, row3Y, SLIDER_CARD_W, TILE_H);
  lv_obj_set_style_bg_color(sliderCard, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_opa(sliderCard, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(sliderCard, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(sliderCard, 1, 0);
  lv_obj_set_style_radius(sliderCard, 12, 0);

  lv_obj_t* sldTitle = lv_label_create(sliderCard);
  lv_label_set_text(sldTitle, LV_SYMBOL_EYE_OPEN "  Skala UI");
  lv_obj_set_pos(sldTitle, 12, 10);
  lv_obj_set_style_text_font(sldTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(sldTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_lblScaleVal = lv_label_create(sliderCard);
  lv_label_set_text(s_lblScaleVal, "100%");
  lv_obj_set_pos(s_lblScaleVal, SLIDER_CARD_W - 56, 11);
  lv_obj_set_width(s_lblScaleVal, 44);
  lv_obj_set_style_text_align(s_lblScaleVal, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_font(s_lblScaleVal, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblScaleVal, lv_color_hex(StudioTheme::CHAMPAGNE), 0);

  s_sldScale = lv_slider_create(sliderCard);
  lv_obj_set_pos(s_sldScale, 14, 38);
  lv_obj_set_size(s_sldScale, SLIDER_CARD_W - 28, 14);
  lv_slider_set_range(s_sldScale, 60, 110);
  lv_slider_set_value(s_sldScale, uiSettingsGet().scalePct, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s_sldScale, lv_color_hex(StudioTheme::TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_sldScale, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_sldScale, 7, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_sldScale, lv_color_hex(StudioTheme::CHAMPAGNE), LV_PART_INDICATOR);
  lv_obj_set_style_radius(s_sldScale, 7, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_sldScale, lv_color_hex(StudioTheme::TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(s_sldScale, 2, LV_PART_KNOB);
  lv_obj_add_event_cb(s_sldScale, onScaleSliderEvent, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_sldScale, onScaleSliderEvent, LV_EVENT_RELEASED, nullptr);

  createShortcutTile(
    s_widgetView, SHORTCUT_AREA_X, row3Y, SHORTCUT_W, TILE_H,
    LV_SYMBOL_SD_CARD, "Docker", StudioTheme::TEAL, onOpenDockerClick
  );
  createShortcutTile(
    s_widgetView, SHORTCUT_AREA_X + SHORTCUT_W + GAP, row3Y, SHORTCUT_W, TILE_H,
    LV_SYMBOL_SETTINGS, "Settings", StudioTheme::CHAMPAGNE, onOpenSettingsClick
  );
}

static void gesturePollTimerCb(lv_timer_t* /*t*/) {
  if (bootScreenIsActive() || !s_shade) return;

  lv_indev_t* indev = lv_indev_get_next(nullptr);
  if (!indev) return;

  lv_point_t pt;
  lv_indev_get_point(indev, &pt);
  bool pressed = (indev->proc.state == LV_INDEV_STATE_PRESSED);

  if (pressed) {
    if (!s_tracking) {
      s_tracking = true;
      s_dragging = false;
      s_startX   = pt.x;
      s_startY   = pt.y;
      s_lastDy   = 0;
    } else if (!s_animating) {
      int16_t dx    = pt.x - s_startX;
      int16_t dy    = pt.y - s_startY;
      int16_t absDx = dx < 0 ? -dx : dx;
      int16_t absDy = dy < 0 ? -dy : dy;
      s_lastDy      = dy;

      if (!s_isOpen) {
        if (!s_dragging && s_startY <= SWIPE_START_TOP_MAX_Y && dy >= 12 && absDy > absDx) {
          s_dragging = true;
          // Belah sisi kiri (Notifikasi) vs sisi kanan (Quick Widget ala Android)
          ShadeMode targetMode = (s_startX < (UI_W / 2)) ? ShadeMode::NOTIF : ShadeMode::WIDGETS;
          applyShadeMode(targetMode);
          lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(s_shade);
          showHintPills(false);
        }
        if (s_dragging) {
          int newY = -UI_H + dy;
          if (newY > 0) newY = 0;
          if (newY < -UI_H) newY = -UI_H;
          lv_obj_set_y(s_shade, newY);
        }
      } else {
        if (!s_dragging && dy <= -12 && absDy > absDx) {
          bool allowClose = false;
          if (s_mode == ShadeMode::NOTIF) {
            allowClose = (s_startY >= UI_H - 64) || !s_list || (lv_obj_get_scroll_bottom(s_list) <= 2);
          } else {
            // Hindari menutup shade saat menggeser slider Skala UI (y ~ 210..278, x < 292)
            bool onSlider = (s_startY >= 200 && s_startY <= 284 && s_startX < 292);
            allowClose = (s_startY >= UI_H - 48) || !onSlider;
          }
          if (allowClose) {
            s_dragging = true;
          }
        }
        if (s_dragging) {
          int newY = dy;
          if (newY > 0) newY = 0;
          if (newY < -UI_H) newY = -UI_H;
          lv_obj_set_y(s_shade, newY);
        }
      }
    }
  } else {
    if (s_tracking && s_dragging && !s_animating) {
      if (!s_isOpen) {
        if (s_lastDy >= SWIPE_OPEN_THRESHOLD_Y) {
          if (s_mode == ShadeMode::WIDGETS) notificationShadeOpenWidgets();
          else                              notificationShadeOpen();
        } else {
          s_isOpen = true;
          notificationShadeClose();
        }
      } else {
        if (s_lastDy <= -SWIPE_CLOSE_THRESHOLD_Y) {
          notificationShadeClose();
        } else {
          s_isOpen = false;
          if (s_mode == ShadeMode::WIDGETS) notificationShadeOpenWidgets();
          else                              notificationShadeOpen();
        }
      }
    }
    s_tracking = false;
    s_dragging = false;
    s_lastDy   = 0;
  }
}

static void openShadeWithMode(ShadeMode mode) {
  if (!s_shade) return;
  applyShadeMode(mode);
  if (s_isOpen && !s_dragging) return;

  s_isOpen    = true;
  s_animating = true;

  showHintPills(false);
  lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_shade);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_shade);
  lv_anim_set_exec_cb(&a, animYCb);
  lv_anim_set_values(&a, lv_obj_get_y(s_shade), 0);
  lv_anim_set_time(&a, SHADE_ANIM_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, onOpenAnimReady);
  lv_anim_start(&a);
}

void notificationShadeInit() {
  lv_obj_t* topLayer = lv_layer_top();

  // Satu indikator pill tipis di tengah atas layar sebagai petunjuk swipe-down
  s_topHintPill = makePlainBox(topLayer, (UI_W - 44) / 2, 4, 44, 4);
  lv_obj_set_style_bg_color(s_topHintPill, lv_color_hex(StudioTheme::SUBTLE), 0);
  lv_obj_set_style_bg_opa(s_topHintPill, LV_OPA_70, 0);
  lv_obj_set_style_radius(s_topHintPill, 2, 0);

  // Container utama shade (full screen di layer teratas)
  s_shade = makePlainBox(topLayer, 0, -UI_H, UI_W, UI_H);
  lv_obj_set_style_bg_color(s_shade, lv_color_hex(StudioTheme::BG), 0);
  lv_obj_set_style_bg_opa(s_shade, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_shade, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);

  // Header shade
  lv_obj_t* header = makePlainBox(s_shade, 0, 0, UI_W, HEADER_H);
  lv_obj_set_style_bg_color(header, lv_color_hex(StudioTheme::HEADER), 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(header, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(header, 1, 0);
  lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);

  s_hdrIcon = lv_label_create(header);
  lv_label_set_text(s_hdrIcon, LV_SYMBOL_BELL);
  lv_obj_set_pos(s_hdrIcon, 14, 13);
  lv_obj_set_style_text_font(s_hdrIcon, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_hdrIcon, lv_color_hex(StudioTheme::CHAMPAGNE), 0);

  s_hdrTitle = lv_label_create(header);
  lv_label_set_text(s_hdrTitle, "Notifikasi");
  lv_obj_set_pos(s_hdrTitle, 38, 12);
  lv_obj_set_style_text_font(s_hdrTitle, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_hdrTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_hdrBadge = lv_label_create(header);
  lv_label_set_text(s_hdrBadge, "0");
  lv_obj_set_pos(s_hdrBadge, 162, 14);
  lv_obj_set_style_text_font(s_hdrBadge, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_hdrBadge, lv_color_hex(StudioTheme::MUTED), 0);

  // Segmented pill switcher: Notif | Widget
  lv_obj_t* segBox = makePlainBox(header, 232, 7, 134, 30);
  lv_obj_set_style_bg_color(segBox, lv_color_hex(StudioTheme::TRACK), 0);
  lv_obj_set_style_bg_opa(segBox, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(segBox, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(segBox, 1, 0);
  lv_obj_set_style_radius(segBox, 8, 0);

  s_tabNotifBtn = lv_btn_create(segBox);
  lv_obj_remove_style_all(s_tabNotifBtn);
  lv_obj_set_pos(s_tabNotifBtn, 2, 2);
  lv_obj_set_size(s_tabNotifBtn, 63, 24);
  lv_obj_set_style_bg_opa(s_tabNotifBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_tabNotifBtn, 6, 0);
  lv_obj_add_event_cb(s_tabNotifBtn, onTabNotifClick, LV_EVENT_CLICKED, nullptr);

  s_tabNotifLbl = lv_label_create(s_tabNotifBtn);
  lv_label_set_text(s_tabNotifLbl, "Notif");
  lv_obj_set_style_text_font(s_tabNotifLbl, &lv_font_montserrat_12, 0);
  lv_obj_center(s_tabNotifLbl);

  s_tabWidgetBtn = lv_btn_create(segBox);
  lv_obj_remove_style_all(s_tabWidgetBtn);
  lv_obj_set_pos(s_tabWidgetBtn, 67, 2);
  lv_obj_set_size(s_tabWidgetBtn, 63, 24);
  lv_obj_set_style_bg_opa(s_tabWidgetBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_tabWidgetBtn, 6, 0);
  lv_obj_add_event_cb(s_tabWidgetBtn, onTabWidgetClick, LV_EVENT_CLICKED, nullptr);

  s_tabWidgetLbl = lv_label_create(s_tabWidgetBtn);
  lv_label_set_text(s_tabWidgetLbl, "Widget");
  lv_obj_set_style_text_font(s_tabWidgetLbl, &lv_font_montserrat_12, 0);
  lv_obj_center(s_tabWidgetLbl);

  // Tombol "Hapus" (hanya muncul di mode Notifikasi saat ada notif)
  s_clearBtn = lv_btn_create(header);
  lv_obj_remove_style_all(s_clearBtn);
  lv_obj_set_pos(s_clearBtn, UI_W - 106, 8);
  lv_obj_set_size(s_clearBtn, 56, 28);
  lv_obj_set_style_bg_color(s_clearBtn, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(s_clearBtn, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(s_clearBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_clearBtn, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(s_clearBtn, 1, 0);
  lv_obj_set_style_radius(s_clearBtn, 8, 0);
  lv_obj_add_event_cb(s_clearBtn, onClearAllClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* clearLbl = lv_label_create(s_clearBtn);
  lv_label_set_text(clearLbl, "Hapus");
  lv_obj_set_style_text_font(clearLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(clearLbl, lv_color_hex(StudioTheme::TEXT), 0);
  lv_obj_center(clearLbl);

  // Tombol tutup ("X")
  lv_obj_t* closeBtn = lv_btn_create(header);
  lv_obj_remove_style_all(closeBtn);
  lv_obj_set_pos(closeBtn, UI_W - 44, 8);
  lv_obj_set_size(closeBtn, 34, 28);
  lv_obj_set_style_bg_color(closeBtn, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(closeBtn, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(closeBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(closeBtn, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(closeBtn, 1, 0);
  lv_obj_set_style_radius(closeBtn, 8, 0);
  lv_obj_add_event_cb(closeBtn, onCloseClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* closeLbl = lv_label_create(closeBtn);
  lv_label_set_text(closeLbl, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(closeLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(closeLbl, lv_color_hex(StudioTheme::MUTED), 0);
  lv_obj_center(closeLbl);

  // 1) Area Notifikasi (kiri swipe)
  s_list = lv_obj_create(s_shade);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_pos(s_list, 0, HEADER_H);
  lv_obj_set_size(s_list, UI_W, BODY_H);
  lv_obj_set_layout(s_list, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(s_list, 12, 0);
  lv_obj_set_style_pad_row(s_list, 8, 0);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

  s_emptyBox = makePlainBox(s_list, 0, 0, UI_W - 24, BODY_H - 24);
  lv_obj_set_layout(s_emptyBox, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_emptyBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_emptyBox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_emptyBox, 10, 0);

  lv_obj_t* emptyIcon = lv_label_create(s_emptyBox);
  lv_label_set_text(emptyIcon, LV_SYMBOL_BELL);
  lv_obj_set_style_text_font(emptyIcon, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(emptyIcon, lv_color_hex(StudioTheme::SUBTLE), 0);

  lv_obj_t* emptyText = lv_label_create(s_emptyBox);
  lv_label_set_text(emptyText, "Tidak ada notifikasi");
  lv_obj_set_style_text_font(emptyText, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(emptyText, lv_color_hex(StudioTheme::MUTED), 0);

  // 2) Area Quick Widget Android (kanan swipe)
  buildWidgetView();

  // Handle bar di bagian bawah shade
  lv_obj_t* bottomBar = lv_btn_create(s_shade);
  lv_obj_remove_style_all(bottomBar);
  lv_obj_set_pos(bottomBar, 0, UI_H - BOTTOM_BAR_H);
  lv_obj_set_size(bottomBar, UI_W, BOTTOM_BAR_H);
  lv_obj_set_style_bg_color(bottomBar, lv_color_hex(StudioTheme::FOOTER), 0);
  lv_obj_set_style_bg_opa(bottomBar, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(bottomBar, onCloseClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* handlePill = makePlainBox(bottomBar, (UI_W - 56) / 2, (BOTTOM_BAR_H - 4) / 2, 56, 4);
  lv_obj_set_style_bg_color(handlePill, lv_color_hex(StudioTheme::MUTED), 0);
  lv_obj_set_style_bg_opa(handlePill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(handlePill, 2, 0);

  rebuildNotificationList();
  applyShadeMode(ShadeMode::NOTIF);

  lv_timer_create(gesturePollTimerCb, 20, nullptr);
  s_widgetRefreshTimer = lv_timer_create(widgetRefreshTimerCb, 1000, nullptr);
}

void notificationShadeOpen() {
  openShadeWithMode(ShadeMode::NOTIF);
}

void notificationShadeOpenWidgets() {
  openShadeWithMode(ShadeMode::WIDGETS);
}

void notificationShadeClose() {
  if (!s_shade || !s_isOpen) return;
  s_isOpen    = false;
  s_animating = true;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_shade);
  lv_anim_set_exec_cb(&a, animYCb);
  lv_anim_set_values(&a, lv_obj_get_y(s_shade), -UI_H);
  lv_anim_set_time(&a, SHADE_ANIM_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, onCloseAnimReady);
  lv_anim_start(&a);
}

bool notificationShadeIsOpen() {
  return s_isOpen;
}

void notificationPush(const char* title,
                      const char* body,
                      const char* timeStr,
                      uint32_t accentHex,
                      const char* syncKey) {
  char cleanTitle[48];
  char cleanBody[128];
  sanitizeUtf8ForLvgl(title ? title : "Notifikasi", cleanTitle, sizeof(cleanTitle));
  sanitizeUtf8ForLvgl(body ? body : "", cleanBody, sizeof(cleanBody));
  if (cleanTitle[0] == '\0') snprintf(cleanTitle, sizeof(cleanTitle), "Notifikasi");

  if (syncKey && syncKey[0] != '\0') {
    for (int i = 0; i < s_count; i++) {
      if (strcmp(s_items[i].key, syncKey) == 0) {
        snprintf(s_items[i].title, sizeof(s_items[i].title), "%s", cleanTitle);
        snprintf(s_items[i].body, sizeof(s_items[i].body), "%s", cleanBody);
        snprintf(s_items[i].timeStr, sizeof(s_items[i].timeStr), "%s", timeStr ? timeStr : "");
        s_items[i].accentHex = accentHex;
        rebuildNotificationList();
        return;
      }
    }
  }

  if (s_count >= MAX_NOTIFICATIONS) {
    for (int i = 1; i < MAX_NOTIFICATIONS; i++) {
      s_items[i - 1] = s_items[i];
    }
    s_count = MAX_NOTIFICATIONS - 1;
  }

  NotificationItem& item = s_items[s_count++];
  snprintf(item.title, sizeof(item.title), "%s", cleanTitle);
  snprintf(item.body, sizeof(item.body), "%s", cleanBody);
  snprintf(item.timeStr, sizeof(item.timeStr), "%s", timeStr ? timeStr : "");
  snprintf(item.key, sizeof(item.key), "%s", syncKey ? syncKey : "");
  item.accentHex = accentHex;

  rebuildNotificationList();
}

bool notificationRemoveByKey(const char* syncKey) {
  if (!syncKey || syncKey[0] == '\0') return false;
  for (int i = 0; i < s_count; i++) {
    if (strcmp(s_items[i].key, syncKey) == 0) {
      for (int j = i + 1; j < s_count; j++) {
        s_items[j - 1] = s_items[j];
      }
      s_count--;
      rebuildNotificationList();
      return true;
    }
  }
  return false;
}

void notificationClearAll(bool syncToPhone) {
  if (syncToPhone && s_count > 0) {
    mqttNotifySendClearAll();
  }
  s_count = 0;
  rebuildNotificationList();
}

int notificationCount() {
  return s_count;
}
