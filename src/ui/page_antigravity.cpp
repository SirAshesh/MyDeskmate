#include "page_antigravity.h"
#include "icon_antigravity.h"
#include "studio_theme.h"
#include "wifi_config.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static constexpr int MAX_QUOTA_SLOTS = 4;

struct QuotaSlotWidgets {
  lv_obj_t* card;
  lv_obj_t* lblName;
  lv_obj_t* lblPct;
  lv_obj_t* lblReset;
  lv_obj_t* bar;
};

static lv_obj_t*        s_page      = nullptr;
static lv_obj_t*        s_badge     = nullptr;
static lv_obj_t*        s_lblBadge  = nullptr;
static lv_obj_t*        s_lblFooter = nullptr;
static QuotaSlotWidgets s_slots[MAX_QUOTA_SLOTS] = {};

static SemaphoreHandle_t s_httpMutex     = nullptr;
static char              s_httpLine[384] = {};
static char              s_httpErr[64]   = {};
static volatile bool     s_httpHasLine   = false;
static volatile bool     s_httpHasErr    = false;
static volatile bool     s_forceSync     = false;

static bool isWifiConfigured() {
  return WIFI_SSID[0] != '\0' && strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0;
}

static void httpQuotaTask(void* param) {
  (void)param;
  uint32_t lastFetchMs = 0;

  for (;;) {
    uint32_t nowMs = millis();
    bool due = (lastFetchMs == 0) || (nowMs - lastFetchMs >= QUOTA_POLL_MS) || s_forceSync;

    if (WiFi.status() == WL_CONNECTED && due) {
      s_forceSync = false;
      lastFetchMs = nowMs;

      HTTPClient http;
      http.setTimeout(5000);
      if (http.begin(QUOTA_SERVER_URL)) {
        int code = http.GET();
        if (code == HTTP_CODE_OK) {
          String body = http.getString();
          body.trim();
          if (body.startsWith("QUOTA:") && s_httpMutex && xSemaphoreTake(s_httpMutex, pdMS_TO_TICKS(200))) {
            strncpy(s_httpLine, body.c_str(), sizeof(s_httpLine) - 1);
            s_httpLine[sizeof(s_httpLine) - 1] = '\0';
            s_httpHasLine = true;
            xSemaphoreGive(s_httpMutex);
          }
        } else if (s_httpMutex && xSemaphoreTake(s_httpMutex, pdMS_TO_TICKS(200))) {
          snprintf(s_httpErr, sizeof(s_httpErr), "HTTP %d", code);
          s_httpHasErr = true;
          xSemaphoreGive(s_httpMutex);
        }
        http.end();
      }
    } else if (WiFi.status() != WL_CONNECTED && s_forceSync) {
      s_forceSync = false;
      if (s_httpMutex && xSemaphoreTake(s_httpMutex, pdMS_TO_TICKS(200))) {
        snprintf(s_httpErr, sizeof(s_httpErr), "WiFi connecting...");
        s_httpHasErr = true;
        xSemaphoreGive(s_httpMutex);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

static void pollHttpQuotaResult(lv_timer_t* timer) {
  (void)timer;
  if (!s_httpMutex) return;

  char lineCopy[384];
  char errCopy[64];
  bool hasLine = false;
  bool hasErr  = false;

  if (xSemaphoreTake(s_httpMutex, pdMS_TO_TICKS(10))) {
    if (s_httpHasLine) {
      strncpy(lineCopy, s_httpLine, sizeof(lineCopy));
      lineCopy[sizeof(lineCopy) - 1] = '\0';
      s_httpHasLine = false;
      hasLine = true;
    }
    if (s_httpHasErr) {
      strncpy(errCopy, s_httpErr, sizeof(errCopy));
      errCopy[sizeof(errCopy) - 1] = '\0';
      s_httpHasErr = false;
      hasErr = true;
    }
    xSemaphoreGive(s_httpMutex);
  }

  if (hasLine) {
    antigravityHandleSerialLine(lineCopy);
  } else if (hasErr && s_lblFooter) {
    lv_label_set_text_fmt(s_lblFooter, "Sync: %s", errCopy);
  }
}

static uint32_t colorForPct(int pct) {
  if (pct < 0)  return StudioTheme::MUTED;
  if (pct >= 60) return StudioTheme::TEAL;
  if (pct >= 25) return StudioTheme::CHAMPAGNE;
  return StudioTheme::ROSE;
}

static void setSlotData(int idx, const char* name, int pct, const char* resetText) {
  if (idx < 0 || idx >= MAX_QUOTA_SLOTS) return;
  QuotaSlotWidgets& s = s_slots[idx];

  if (!name || !name[0]) {
    lv_label_set_text(s.lblName, "-");
    lv_label_set_text(s.lblPct, "--%");
    lv_obj_set_style_text_color(s.lblPct, lv_color_hex(StudioTheme::MUTED), 0);
    lv_label_set_text(s.lblReset, "No data");
    lv_bar_set_value(s.bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s.bar, lv_color_hex(StudioTheme::MUTED), LV_PART_INDICATOR);
    return;
  }

  int clamped = pct;
  if (clamped < 0) clamped = 0;
  if (clamped > 100) clamped = 100;

  uint32_t accent = colorForPct(clamped);
  lv_label_set_text(s.lblName, name);
  lv_label_set_text_fmt(s.lblPct, "%d%%", clamped);
  lv_obj_set_style_text_color(s.lblPct, lv_color_hex(accent), 0);
  lv_label_set_text(s.lblReset, (resetText && resetText[0]) ? resetText : "Reset: -");
  lv_bar_set_value(s.bar, clamped, LV_ANIM_ON);
  lv_obj_set_style_bg_color(s.bar, lv_color_hex(accent), LV_PART_INDICATOR);
}

static void setBadgeState(const char* text, uint32_t accentHex) {
  if (s_lblBadge) {
    lv_label_set_text(s_lblBadge, text);
    lv_obj_set_style_text_color(s_lblBadge, lv_color_hex(accentHex), 0);
  }
  if (s_badge) {
    lv_obj_set_style_border_color(s_badge, lv_color_hex(accentHex), 0);
  }
}

static void updateFooterTimestamp(const char* prefix) {
  if (!s_lblFooter) return;
  time_t now = time(nullptr);
  struct tm tmNow = {};
  localtime_r(&now, &tmNow);
  char buf[48];
  snprintf(buf, sizeof(buf), "%s %02d:%02d:%02d", prefix, tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
  lv_label_set_text(s_lblFooter, buf);
}

static void loadDemoQuota() {
  setBadgeState("AGY - DEMO", StudioTheme::CHAMPAGNE);
  setSlotData(0, "Gemini 5h",      58, "Reset: 2h 00m");
  setSlotData(1, "Gemini Weekly",  54, "Reset: 5d 11h");
  setSlotData(2, "Claude/GPT",    100, "Reset: 6d 23h");
  setSlotData(3, "Gemini 4 Argon", 79, "Reset: 5d 11h");
  updateFooterTimestamp("Demo preview");
}

static void onDemoBtnClick(lv_event_t* e) {
  (void)e;
  loadDemoQuota();
}

static void onSyncBtnClick(lv_event_t* e) {
  (void)e;
  if (!isWifiConfigured()) {
    if (s_lblFooter) lv_label_set_text(s_lblFooter, "Isi WiFi di include/wifi_config.h");
    return;
  }
  s_forceSync = true;
  if (s_lblFooter) lv_label_set_text(s_lblFooter, "Syncing HTTP...");
}

static lv_obj_t* makeQuotaCard(lv_obj_t* parent, int w, int h, QuotaSlotWidgets& out) {
  lv_obj_t* card = lv_obj_create(parent);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, w, h);
  lv_obj_set_style_bg_color(card, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_pad_hor(card, 12, 0);
  lv_obj_set_style_pad_ver(card, 10, 0);
  lv_obj_set_style_pad_row(card, 4, 0);
  lv_obj_set_layout(card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  out.card = card;

  out.lblName = lv_label_create(card);
  lv_obj_set_width(out.lblName, lv_pct(100));
  lv_label_set_long_mode(out.lblName, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(out.lblName, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(out.lblName, lv_color_hex(StudioTheme::TEXT), 0);

  lv_obj_t* midRow = makePlainBox(card, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_width(midRow, lv_pct(100));
  lv_obj_set_layout(midRow, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(midRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(midRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);

  out.lblPct = lv_label_create(midRow);
  lv_obj_set_style_text_font(out.lblPct, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(out.lblPct, lv_color_hex(StudioTheme::MUTED), 0);

  out.lblReset = lv_label_create(midRow);
  lv_obj_set_style_text_font(out.lblReset, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(out.lblReset, lv_color_hex(StudioTheme::MUTED), 0);

  out.bar = lv_bar_create(card);
  lv_obj_set_size(out.bar, lv_pct(100), 8);
  lv_bar_set_range(out.bar, 0, 100);
  lv_bar_set_value(out.bar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(out.bar, lv_color_hex(StudioTheme::TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(out.bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(out.bar, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(out.bar, lv_color_hex(StudioTheme::TEAL), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(out.bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(out.bar, 4, LV_PART_INDICATOR);

  return card;
}

lv_obj_t* buildAntigravityPage(lv_obj_t* parent, int rightW, int rightH) {
  s_page = makePlainBox(parent, 0, 0, rightW, rightH);
  lv_obj_set_style_pad_hor(s_page, 14, 0);
  lv_obj_set_style_pad_ver(s_page, 12, 0);
  lv_obj_set_style_pad_row(s_page, 10, 0);
  lv_obj_set_layout(s_page, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);

  const int contentW = rightW - 28;

  // Header
  lv_obj_t* header = makePlainBox(s_page, 0, 0, contentW, 34);
  lv_obj_set_layout(header, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* titleGroup = makePlainBox(header, 0, 0, LV_SIZE_CONTENT, 32);
  lv_obj_set_layout(titleGroup, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(titleGroup, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(titleGroup, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(titleGroup, 8, 0);

  lv_obj_t* hdrIcon = lv_img_create(titleGroup);
  lv_img_set_src(hdrIcon, &icon_antigravity);

  lv_obj_t* lblTitle = lv_label_create(titleGroup);
  lv_label_set_text(lblTitle, "Antigravity Quota");
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_badge = lv_obj_create(header);
  lv_obj_remove_style_all(s_badge);
  lv_obj_set_size(s_badge, LV_SIZE_CONTENT, 24);
  lv_obj_set_style_bg_color(s_badge, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_opa(s_badge, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_badge, lv_color_hex(StudioTheme::MUTED), 0);
  lv_obj_set_style_border_width(s_badge, 1, 0);
  lv_obj_set_style_radius(s_badge, 12, 0);
  lv_obj_set_style_pad_hor(s_badge, 10, 0);
  lv_obj_set_style_pad_ver(s_badge, 3, 0);

  s_lblBadge = lv_label_create(s_badge);
  lv_label_set_text(s_lblBadge, "WAITING");
  lv_obj_set_style_text_font(s_lblBadge, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblBadge, lv_color_hex(StudioTheme::MUTED), 0);
  lv_obj_center(s_lblBadge);

  // 2x2 Grid container
  const int gridH = rightH - 24 - 34 - 28 - 20; // ~214px
  lv_obj_t* grid = makePlainBox(s_page, 0, 0, contentW, gridH);
  lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_BETWEEN);
  lv_obj_set_style_pad_column(grid, 10, 0);
  lv_obj_set_style_pad_row(grid, 10, 0);

  const int cardW = (contentW - 10) / 2; // ~186px
  const int cardH = (gridH - 10) / 2;    // ~102px
  for (int i = 0; i < MAX_QUOTA_SLOTS; i++) {
    makeQuotaCard(grid, cardW, cardH, s_slots[i]);
  }

  // Initial placeholder values
  setSlotData(0, "Gemini 5h",      -1, "Waiting...");
  setSlotData(1, "Gemini Weekly",  -1, "Waiting...");
  setSlotData(2, "Claude/GPT",     -1, "Waiting...");
  setSlotData(3, "Gemini 4 Argon", -1, "Waiting...");

  // Footer
  lv_obj_t* footer = makePlainBox(s_page, 0, 0, contentW, 28);
  lv_obj_set_layout(footer, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  s_lblFooter = lv_label_create(footer);
  lv_label_set_text(
    s_lblFooter,
    isWifiConfigured() ? "WiFi HTTP: connecting..." : "Set WiFi di include/wifi_config.h"
  );
  lv_obj_set_style_text_font(s_lblFooter, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblFooter, lv_color_hex(StudioTheme::SUBTLE), 0);

  lv_obj_t* btnRow = makePlainBox(footer, 0, 0, LV_SIZE_CONTENT, 24);
  lv_obj_set_layout(btnRow, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(btnRow, 6, 0);

  lv_obj_t* btnSync = lv_btn_create(btnRow);
  lv_obj_remove_style_all(btnSync);
  lv_obj_set_size(btnSync, 54, 24);
  lv_obj_set_style_bg_color(btnSync, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(btnSync, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btnSync, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btnSync, lv_color_hex(StudioTheme::TEAL), 0);
  lv_obj_set_style_border_width(btnSync, 1, 0);
  lv_obj_set_style_radius(btnSync, 6, 0);
  lv_obj_add_event_cb(btnSync, onSyncBtnClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lblSync = lv_label_create(btnSync);
  lv_label_set_text(lblSync, "Sync");
  lv_obj_set_style_text_font(lblSync, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblSync, lv_color_hex(StudioTheme::TEAL), 0);
  lv_obj_center(lblSync);

  lv_obj_t* btnDemo = lv_btn_create(btnRow);
  lv_obj_remove_style_all(btnDemo);
  lv_obj_set_size(btnDemo, 54, 24);
  lv_obj_set_style_bg_color(btnDemo, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(btnDemo, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btnDemo, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btnDemo, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(btnDemo, 1, 0);
  lv_obj_set_style_radius(btnDemo, 6, 0);
  lv_obj_add_event_cb(btnDemo, onDemoBtnClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lblDemo = lv_label_create(btnDemo);
  lv_label_set_text(lblDemo, "Demo");
  lv_obj_set_style_text_font(lblDemo, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblDemo, lv_color_hex(StudioTheme::CHAMPAGNE), 0);
  lv_obj_center(lblDemo);

  if (isWifiConfigured()) {
    s_httpMutex = xSemaphoreCreateMutex();
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    xTaskCreatePinnedToCore(httpQuotaTask, "quota_http", 6144, nullptr, 1, nullptr, 0);
    lv_timer_create(pollHttpQuotaResult, 250, nullptr);
  }

  return s_page;
}

// Format:
//   QUOTA:DEMO
//   QUOTA:<plan>|<status>|<name1>:<pct1>:<reset1>;<name2>:<pct2>:<reset2>;...
void antigravityHandleSerialLine(const char* line) {
  if (!line || !s_page) return;
  if (strncmp(line, "QUOTA:", 6) != 0) return;

  const char* payload = line + 6;
  if (strcmp(payload, "DEMO") == 0) {
    loadDemoQuota();
    Serial.println("[QUOTA] Loaded demo data");
    return;
  }

  char buf[384];
  strncpy(buf, payload, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* p1 = strchr(buf, '|');
  char* p2 = p1 ? strchr(p1 + 1, '|') : nullptr;
  if (!p1 || !p2) {
    Serial.println("[QUOTA] Invalid format. Use QUOTA:<plan>|<status>|<name>:<pct>:<reset>;...");
    return;
  }

  *p1 = '\0';
  *p2 = '\0';
  const char* plan   = buf;
  const char* status = p1 + 1;
  char*       items  = p2 + 1;

  char badgeText[32];
  if (plan[0] && status[0]) snprintf(badgeText, sizeof(badgeText), "%s - %s", plan, status);
  else if (plan[0])         snprintf(badgeText, sizeof(badgeText), "%s", plan);
  else                      snprintf(badgeText, sizeof(badgeText), "LIVE");
  setBadgeState(badgeText, StudioTheme::TEAL);

  int slot = 0;
  char* cursor = items;
  while (cursor && *cursor && slot < MAX_QUOTA_SLOTS) {
    char* next = strchr(cursor, ';');
    if (next) *next = '\0';

    char* c1 = strchr(cursor, ':');
    char* c2 = c1 ? strchr(c1 + 1, ':') : nullptr;
    if (c1) {
      *c1 = '\0';
      int pct = atoi(c1 + 1);
      const char* resetStr = "";
      if (c2) {
        *c2 = '\0';
        pct = atoi(c1 + 1);
        resetStr = c2 + 1;
      }
      setSlotData(slot, cursor, pct, resetStr);
      slot++;
    }
    cursor = next ? (next + 1) : nullptr;
  }

  for (int i = slot; i < MAX_QUOTA_SLOTS; i++) {
    setSlotData(i, "", -1, "");
  }

  updateFooterTimestamp("Updated");
  Serial.printf("[QUOTA] Updated %d slots\n", slot);
}
