#include "page_lyrics.h"
#include "studio_theme.h"
#include "../utils/text_sanitize.h"
#include "wifi_config.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LYRICS_WS_HOST
#define LYRICS_WS_HOST "192.168.100.80"
#endif

#ifndef LYRICS_WS_PORT
#define LYRICS_WS_PORT 8765
#endif

namespace {
constexpr int THUMB_W = 128;
constexpr int THUMB_H = 128;
constexpr size_t THUMB_BYTES = THUMB_W * THUMB_H * 2;

struct LyricsState {
  bool  wsConnected;
  bool  paused;
  bool  hasLyrics;
  float timeSec;
  float durationSec;
  char  videoId[32];
  char  title[96];
  char  artist[64];
  char  prev[128];
  char  curr[128];
  char  next1[128];
  char  next2[128];
};

lv_obj_t* s_pageLyrics   = nullptr;
lv_obj_t* s_viewThumb    = nullptr;
lv_obj_t* s_viewLyrics   = nullptr;
lv_obj_t* s_imgThumb     = nullptr;
lv_obj_t* s_lblThumbIcon = nullptr;
lv_obj_t* s_lblTitle     = nullptr;
lv_obj_t* s_lblArtist    = nullptr;
lv_obj_t* s_lblStatus    = nullptr;
lv_obj_t* s_badgeStatus  = nullptr;

lv_obj_t* s_lblLyrTitle  = nullptr;
lv_obj_t* s_lblLyricPrev = nullptr;
lv_obj_t* s_lblLyricCurr = nullptr;
lv_obj_t* s_lblLyricNext1 = nullptr;
lv_obj_t* s_lblLyricNext2 = nullptr;

lv_obj_t* s_lblCurTime   = nullptr;
lv_obj_t* s_lblTotTime   = nullptr;
lv_obj_t* s_barProgress  = nullptr;
lv_obj_t* s_lblPlayPause = nullptr;
lv_obj_t* s_btnLyrics    = nullptr;
lv_obj_t* s_lblLyricsBtn = nullptr;

bool s_showLyrics = false;

SemaphoreHandle_t s_stateMutex = nullptr;
QueueHandle_t     s_cmdQueue   = nullptr;
LyricsState       s_shared     = {};
volatile bool     s_dirty      = false;
volatile bool     s_thumbReady = false;
volatile bool     s_hasThumb   = false;

uint8_t*          s_thumbBuf   = nullptr;
uint8_t*          s_thumbStage = nullptr;
lv_img_dsc_t      s_thumbDsc   = {};
char              s_loadedThumbVid[32] = {};

WebSocketsClient  s_ws;
bool              s_wsStarted  = false;
} // namespace

static String extractJsonStr(const String& json, const char* key) {
  String pattern = String("\"") + key + "\"";
  int idx = json.indexOf(pattern);
  if (idx < 0) return "";
  int colon = json.indexOf(':', idx + pattern.length());
  if (colon < 0) return "";
  int p = colon + 1;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  if (p >= (int)json.length() || json[p] != '"') return "";
  int q1 = p;
  int q2 = q1 + 1;
  while (q2 < (int)json.length()) {
    if (json[q2] == '"' && json[q2 - 1] != '\\') break;
    q2++;
  }
  if (q2 >= (int)json.length()) return "";
  String val = json.substring(q1 + 1, q2);
  val.replace("\\\"", "\"");
  val.replace("\\n", " ");
  return val;
}

static float extractJsonFloat(const String& json, const char* key, float fallback = 0.0f) {
  String pattern = String("\"") + key + "\"";
  int idx = json.indexOf(pattern);
  if (idx < 0) return fallback;
  int colon = json.indexOf(':', idx + pattern.length());
  if (colon < 0) return fallback;
  int p = colon + 1;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  int end = p;
  while (end < (int)json.length() && (isdigit(json[end]) || json[end] == '.' || json[end] == '-')) {
    end++;
  }
  if (end <= p) return fallback;
  return json.substring(p, end).toFloat();
}

static bool extractJsonBool(const String& json, const char* key, bool fallback = false) {
  String pattern = String("\"") + key + "\"";
  int idx = json.indexOf(pattern);
  if (idx < 0) return fallback;
  int colon = json.indexOf(':', idx + pattern.length());
  if (colon < 0) return fallback;
  int p = colon + 1;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  if (json.startsWith("true", p)) return true;
  if (json.startsWith("false", p)) return false;
  return fallback;
}

static void fetchThumbnailSync(const char* vid) {
  if (!vid || vid[0] == '\0' || !s_thumbStage || !s_thumbBuf) return;
  if (strcmp(s_loadedThumbVid, vid) == 0) return;

  HTTPClient http;
  char url[160];
  snprintf(url, sizeof(url), "http://%s:%d/thumb.rgb565?v=%s", LYRICS_WS_HOST, LYRICS_WS_PORT, vid);
  http.setTimeout(5000);
  if (!http.begin(url)) return;

  int code = http.GET();
  if (code == HTTP_CODE_OK && http.getSize() == (int)THUMB_BYTES) {
    WiFiClient* stream = http.getStreamPtr();
    size_t got = 0;
    uint32_t startMs = millis();
    while (got < THUMB_BYTES && (millis() - startMs) < 5000) {
      size_t avail = stream->available();
      if (avail > 0) {
        size_t toRead = THUMB_BYTES - got;
        if (avail < toRead) toRead = avail;
        int r = stream->readBytes(s_thumbStage + got, toRead);
        if (r > 0) got += (size_t)r;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    if (got == THUMB_BYTES && s_stateMutex && xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(200))) {
      memcpy(s_thumbBuf, s_thumbStage, THUMB_BYTES);
      strncpy(s_loadedThumbVid, vid, sizeof(s_loadedThumbVid) - 1);
      s_loadedThumbVid[sizeof(s_loadedThumbVid) - 1] = '\0';
      s_hasThumb   = true;
      s_thumbReady = true;
      xSemaphoreGive(s_stateMutex);
    }
  }
  http.end();
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      s_ws.sendTXT("{\"role\":\"esp32\"}");
      if (s_stateMutex && xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(100))) {
        s_shared.wsConnected = true;
        s_dirty = true;
        xSemaphoreGive(s_stateMutex);
      }
      break;

    case WStype_DISCONNECTED:
      if (s_stateMutex && xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(100))) {
        s_shared.wsConnected = false;
        s_dirty = true;
        xSemaphoreGive(s_stateMutex);
      }
      break;

    case WStype_TEXT: {
      String raw;
      raw.reserve(length + 1);
      for (size_t i = 0; i < length; i++) raw += (char)payload[i];

      String vid   = extractJsonStr(raw, "videoId");
      String title = extractJsonStr(raw, "title");
      String art   = extractJsonStr(raw, "artist");
      String prev  = extractJsonStr(raw, "prev");
      String curr  = extractJsonStr(raw, "curr");
      String next1 = extractJsonStr(raw, "next1");
      String next2 = extractJsonStr(raw, "next2");
      float  tSec  = extractJsonFloat(raw, "time", 0.0f);
      float  dSec  = extractJsonFloat(raw, "duration", 0.0f);
      bool   paused = extractJsonBool(raw, "paused", true);
      bool   hasLyr = extractJsonBool(raw, "hasLyrics", false);

      if (s_stateMutex && xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(100))) {
        s_shared.wsConnected = true;
        s_shared.paused      = paused;
        s_shared.hasLyrics   = hasLyr;
        s_shared.timeSec     = tSec;
        s_shared.durationSec = dSec;
        snprintf(s_shared.videoId, sizeof(s_shared.videoId), "%s", vid.c_str());
        sanitizeUtf8ForLvgl(title.length() ? title.c_str() : "Tidak ada lagu",
                            s_shared.title, sizeof(s_shared.title));
        sanitizeUtf8ForLvgl(art.length() ? art.c_str() : "-",
                            s_shared.artist, sizeof(s_shared.artist));
        sanitizeUtf8ForLvgl(prev.c_str(),  s_shared.prev,  sizeof(s_shared.prev));
        sanitizeUtf8ForLvgl(curr.c_str(),  s_shared.curr,  sizeof(s_shared.curr));
        sanitizeUtf8ForLvgl(next1.c_str(), s_shared.next1, sizeof(s_shared.next1));
        sanitizeUtf8ForLvgl(next2.c_str(), s_shared.next2, sizeof(s_shared.next2));
        s_dirty = true;
        xSemaphoreGive(s_stateMutex);
      }
      break;
    }

    default:
      break;
  }
}

static void lyricsWsTask(void* /*param*/) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!s_wsStarted) {
        s_ws.begin(LYRICS_WS_HOST, LYRICS_WS_PORT, "/ws");
        s_ws.onEvent(onWsEvent);
        s_ws.setReconnectInterval(3000);
        s_wsStarted = true;
      }
      s_ws.loop();

      char cmdBuf[16];
      while (s_cmdQueue && xQueueReceive(s_cmdQueue, cmdBuf, 0) == pdTRUE) {
        char json[48];
        snprintf(json, sizeof(json), "{\"cmd\":\"%s\"}", cmdBuf);
        s_ws.sendTXT(json);
      }

      char targetVid[32] = {};
      if (s_stateMutex && xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(50))) {
        strncpy(targetVid, s_shared.videoId, sizeof(targetVid) - 1);
        xSemaphoreGive(s_stateMutex);
      }
      if (targetVid[0] != '\0' && strcmp(targetVid, s_loadedThumbVid) != 0) {
        fetchThumbnailSync(targetVid);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

static void formatMmSs(float sec, char* buf, size_t len) {
  if (sec < 0.0f) sec = 0.0f;
  uint32_t total = (uint32_t)sec;
  snprintf(buf, len, "%02lu:%02lu", (unsigned long)(total / 60UL), (unsigned long)(total % 60UL));
}

static void sendPlayerCmd(const char* cmd) {
  if (!s_cmdQueue || !cmd) return;
  char buf[16] = {};
  snprintf(buf, sizeof(buf), "%s", cmd);
  xQueueSend(s_cmdQueue, buf, 0);
}

static void onPrevClick(lv_event_t* /*e*/)   { sendPlayerCmd("prev"); }
static void onToggleClick(lv_event_t* /*e*/) { sendPlayerCmd("toggle"); }
static void onNextClick(lv_event_t* /*e*/)   { sendPlayerCmd("next"); }

static void applyViewMode() {
  if (!s_viewThumb || !s_viewLyrics || !s_btnLyrics || !s_lblLyricsBtn) return;
  if (s_showLyrics) {
    lv_obj_add_flag(s_viewThumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_viewLyrics, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(s_btnLyrics, lv_color_hex(StudioTheme::CHAMPAGNE), 0);
    lv_obj_set_style_text_color(s_lblLyricsBtn, lv_color_hex(StudioTheme::CHAMPAGNE), 0);
  } else {
    lv_obj_add_flag(s_viewLyrics, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_viewThumb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(s_btnLyrics, lv_color_hex(StudioTheme::RULE), 0);
    lv_obj_set_style_text_color(s_lblLyricsBtn, lv_color_hex(StudioTheme::MUTED), 0);
  }
}

static void onToggleLyricsViewClick(lv_event_t* /*e*/) {
  s_showLyrics = !s_showLyrics;
  applyViewMode();
}

static void updateLyricsUiTimer(lv_timer_t* /*t*/) {
  if (!s_pageLyrics) return;

  if (s_thumbReady && s_stateMutex && xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(20))) {
    s_thumbReady = false;
    bool hasT = s_hasThumb;
    xSemaphoreGive(s_stateMutex);

    if (hasT && s_imgThumb) {
      lv_img_set_src(s_imgThumb, &s_thumbDsc);
      lv_obj_clear_flag(s_imgThumb, LV_OBJ_FLAG_HIDDEN);
      lv_obj_invalidate(s_imgThumb);
      if (s_lblThumbIcon) lv_obj_add_flag(s_lblThumbIcon, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (!s_dirty) return;
  LyricsState snap = {};
  if (!s_stateMutex || !xSemaphoreTake(s_stateMutex, pdMS_TO_TICKS(20))) return;
  snap = s_shared;
  s_dirty = false;
  xSemaphoreGive(s_stateMutex);

  if (s_lblTitle)    lv_label_set_text(s_lblTitle, snap.title);
  if (s_lblLyrTitle) lv_label_set_text(s_lblLyrTitle, snap.title);
  if (s_lblArtist)   lv_label_set_text(s_lblArtist, snap.artist);

  if (s_lblStatus && s_badgeStatus) {
    if (!snap.wsConnected) {
      lv_label_set_text(s_lblStatus, "WS: Offline");
      lv_obj_set_style_text_color(s_lblStatus, lv_color_hex(StudioTheme::MUTED), 0);
      lv_obj_set_style_border_color(s_badgeStatus, lv_color_hex(StudioTheme::RULE), 0);
    } else if (snap.paused) {
      lv_label_set_text(s_lblStatus, "Dijeda");
      lv_obj_set_style_text_color(s_lblStatus, lv_color_hex(StudioTheme::CORAL), 0);
      lv_obj_set_style_border_color(s_badgeStatus, lv_color_hex(StudioTheme::CORAL), 0);
    } else {
      lv_label_set_text(s_lblStatus, "Memutar");
      lv_obj_set_style_text_color(s_lblStatus, lv_color_hex(StudioTheme::TEAL), 0);
      lv_obj_set_style_border_color(s_badgeStatus, lv_color_hex(StudioTheme::TEAL), 0);
    }
  }

  if (s_lblPlayPause) {
    lv_label_set_text(s_lblPlayPause, snap.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  }

  char tBuf[12];
  formatMmSs(snap.timeSec, tBuf, sizeof(tBuf));
  if (s_lblCurTime) lv_label_set_text(s_lblCurTime, tBuf);
  formatMmSs(snap.durationSec, tBuf, sizeof(tBuf));
  if (s_lblTotTime) lv_label_set_text(s_lblTotTime, tBuf);

  int32_t pct = 0;
  if (snap.durationSec > 0.5f) {
    pct = (int32_t)((snap.timeSec / snap.durationSec) * 1000.0f);
    if (pct < 0) pct = 0;
    if (pct > 1000) pct = 1000;
  }
  if (s_barProgress) lv_bar_set_value(s_barProgress, pct, LV_ANIM_OFF);

  if (s_lblLyricPrev)  lv_label_set_text(s_lblLyricPrev,  snap.prev);
  if (s_lblLyricCurr)  lv_label_set_text(s_lblLyricCurr,  snap.curr[0] ? snap.curr : "Menunggu lagu dari YouTube...");
  if (s_lblLyricNext1) lv_label_set_text(s_lblLyricNext1, snap.next1);
  if (s_lblLyricNext2) lv_label_set_text(s_lblLyricNext2, snap.next2);
}

static lv_obj_t* makeCtrlBtn(lv_obj_t* parent, int w, int h, const char* sym,
                             uint32_t borderHex, uint32_t txtHex, lv_event_cb_t cb,
                             lv_obj_t** outLbl = nullptr) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(borderHex), 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_radius(btn, 10, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, sym);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(txtHex), 0);
  lv_obj_center(lbl);
  if (outLbl) *outLbl = lbl;
  return btn;
}

lv_obj_t* buildLyricsPage(lv_obj_t* parent, int rightW, int rightH) {
  s_stateMutex = xSemaphoreCreateMutex();
  s_cmdQueue   = xQueueCreate(8, 16);

  s_thumbBuf   = (uint8_t*)heap_caps_malloc(THUMB_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_thumbBuf) s_thumbBuf = (uint8_t*)malloc(THUMB_BYTES);
  s_thumbStage = (uint8_t*)heap_caps_malloc(THUMB_BYTES, MALLOC_CAP_SPIRAM);
  if (!s_thumbStage) s_thumbStage = (uint8_t*)malloc(THUMB_BYTES);

  s_thumbDsc.header.always_zero = 0;
  s_thumbDsc.header.w           = THUMB_W;
  s_thumbDsc.header.h           = THUMB_H;
  s_thumbDsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
  s_thumbDsc.data_size          = THUMB_BYTES;
  s_thumbDsc.data               = s_thumbBuf;

  snprintf(s_shared.title, sizeof(s_shared.title), "Menunggu YouTube...");
  snprintf(s_shared.artist, sizeof(s_shared.artist), "Buka YouTube + Tampermonkey");
  snprintf(s_shared.curr, sizeof(s_shared.curr), "Menunggu lagu dari YouTube...");
  s_shared.paused = true;

  s_pageLyrics = makePlainBox(parent, 0, 0, rightW, rightH);
  lv_obj_set_style_pad_hor(s_pageLyrics, 14, 0);
  lv_obj_set_style_pad_ver(s_pageLyrics, 12, 0);
  lv_obj_set_style_pad_row(s_pageLyrics, 10, 0);
  lv_obj_set_layout(s_pageLyrics, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_pageLyrics, LV_FLEX_FLOW_COLUMN);

  const int contentW = rightW - 28;
  constexpr int TOP_AREA_H = 186;

  // Container atas: menampung s_viewThumb dan s_viewLyrics (saling bergantian)
  lv_obj_t* topSlot = makePlainBox(s_pageLyrics, 0, 0, contentW, TOP_AREA_H);

  // --- MODE 1: Thumbnail + Info Lagu ---
  s_viewThumb = makePlainBox(topSlot, 0, 0, contentW, TOP_AREA_H);
  lv_obj_set_style_bg_color(s_viewThumb, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_opa(s_viewThumb, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_viewThumb, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(s_viewThumb, 1, 0);
  lv_obj_set_style_radius(s_viewThumb, 12, 0);
  lv_obj_set_style_pad_all(s_viewThumb, 14, 0);
  lv_obj_set_style_pad_column(s_viewThumb, 14, 0);
  lv_obj_set_layout(s_viewThumb, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_viewThumb, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_viewThumb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* thumbBox = makePlainBox(s_viewThumb, 0, 0, THUMB_W, THUMB_H);
  lv_obj_set_style_bg_color(thumbBox, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_opa(thumbBox, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(thumbBox, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(thumbBox, 1, 0);
  lv_obj_set_style_radius(thumbBox, 10, 0);
  lv_obj_set_style_clip_corner(thumbBox, true, 0);

  s_lblThumbIcon = lv_label_create(thumbBox);
  lv_label_set_text(s_lblThumbIcon, LV_SYMBOL_AUDIO);
  lv_obj_set_style_text_font(s_lblThumbIcon, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(s_lblThumbIcon, lv_color_hex(StudioTheme::MUTED), 0);
  lv_obj_center(s_lblThumbIcon);

  s_imgThumb = lv_img_create(thumbBox);
  lv_obj_set_size(s_imgThumb, THUMB_W, THUMB_H);
  lv_obj_center(s_imgThumb);
  lv_obj_add_flag(s_imgThumb, LV_OBJ_FLAG_HIDDEN);

  const int infoW = contentW - 28 - THUMB_W - 14;
  lv_obj_t* infoCol = makePlainBox(s_viewThumb, 0, 0, infoW, THUMB_H);
  lv_obj_set_layout(infoCol, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(infoCol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(infoCol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(infoCol, 8, 0);

  s_badgeStatus = makePlainBox(infoCol, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(s_badgeStatus, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_opa(s_badgeStatus, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_badgeStatus, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(s_badgeStatus, 1, 0);
  lv_obj_set_style_radius(s_badgeStatus, 8, 0);
  lv_obj_set_style_pad_hor(s_badgeStatus, 8, 0);
  lv_obj_set_style_pad_ver(s_badgeStatus, 3, 0);

  s_lblStatus = lv_label_create(s_badgeStatus);
  lv_label_set_text(s_lblStatus, "WS: Offline");
  lv_obj_set_style_text_font(s_lblStatus, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblStatus, lv_color_hex(StudioTheme::MUTED), 0);

  s_lblTitle = lv_label_create(infoCol);
  lv_label_set_text(s_lblTitle, s_shared.title);
  lv_obj_set_width(s_lblTitle, infoW);
  lv_label_set_long_mode(s_lblTitle, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_max_height(s_lblTitle, 44, 0);
  lv_obj_set_style_text_font(s_lblTitle, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_lblTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_lblArtist = lv_label_create(infoCol);
  lv_label_set_text(s_lblArtist, s_shared.artist);
  lv_obj_set_width(s_lblArtist, infoW);
  lv_label_set_long_mode(s_lblArtist, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_lblArtist, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblArtist, lv_color_hex(StudioTheme::MUTED), 0);

  // --- MODE 2: Synced Lyrics (Ala Spotify) ---
  s_viewLyrics = makePlainBox(topSlot, 0, 0, contentW, TOP_AREA_H);
  lv_obj_set_style_bg_color(s_viewLyrics, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_opa(s_viewLyrics, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_viewLyrics, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(s_viewLyrics, 1, 0);
  lv_obj_set_style_radius(s_viewLyrics, 12, 0);
  lv_obj_set_style_pad_hor(s_viewLyrics, 14, 0);
  lv_obj_set_style_pad_ver(s_viewLyrics, 10, 0);
  lv_obj_set_style_pad_row(s_viewLyrics, 6, 0);
  lv_obj_set_layout(s_viewLyrics, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_viewLyrics, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_viewLyrics, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_add_flag(s_viewLyrics, LV_OBJ_FLAG_HIDDEN);

  const int lyrW = contentW - 28;
  s_lblLyrTitle = lv_label_create(s_viewLyrics);
  lv_label_set_text(s_lblLyrTitle, s_shared.title);
  lv_obj_set_width(s_lblLyrTitle, lyrW);
  lv_label_set_long_mode(s_lblLyrTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_lblLyrTitle, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_lblLyrTitle, lv_color_hex(StudioTheme::TEAL), 0);

  lv_obj_t* lyrBody = makePlainBox(s_viewLyrics, 0, 0, lyrW, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(lyrBody, 1);
  lv_obj_set_layout(lyrBody, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(lyrBody, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(lyrBody, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(lyrBody, 5, 0);

  s_lblLyricPrev = lv_label_create(lyrBody);
  lv_label_set_text(s_lblLyricPrev, "");
  lv_obj_set_width(s_lblLyricPrev, lyrW);
  lv_label_set_long_mode(s_lblLyricPrev, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_lblLyricPrev, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblLyricPrev, lv_color_hex(StudioTheme::SUBTLE), 0);

  s_lblLyricCurr = lv_label_create(lyrBody);
  lv_label_set_text(s_lblLyricCurr, s_shared.curr);
  lv_obj_set_width(s_lblLyricCurr, lyrW);
  lv_label_set_long_mode(s_lblLyricCurr, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_max_height(s_lblLyricCurr, 52, 0);
  lv_obj_set_style_text_font(s_lblLyricCurr, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_lblLyricCurr, lv_color_hex(StudioTheme::CHAMPAGNE), 0);

  s_lblLyricNext1 = lv_label_create(lyrBody);
  lv_label_set_text(s_lblLyricNext1, "");
  lv_obj_set_width(s_lblLyricNext1, lyrW);
  lv_label_set_long_mode(s_lblLyricNext1, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_lblLyricNext1, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lblLyricNext1, lv_color_hex(StudioTheme::MUTED), 0);

  s_lblLyricNext2 = lv_label_create(lyrBody);
  lv_label_set_text(s_lblLyricNext2, "");
  lv_obj_set_width(s_lblLyricNext2, lyrW);
  lv_label_set_long_mode(s_lblLyricNext2, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_lblLyricNext2, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblLyricNext2, lv_color_hex(StudioTheme::SUBTLE), 0);

  // --- Progress Bar Row ---
  lv_obj_t* progRow = makePlainBox(s_pageLyrics, 0, 0, contentW, 24);
  lv_obj_set_layout(progRow, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(progRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(progRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(progRow, 10, 0);

  s_lblCurTime = lv_label_create(progRow);
  lv_label_set_text(s_lblCurTime, "00:00");
  lv_obj_set_style_text_font(s_lblCurTime, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblCurTime, lv_color_hex(StudioTheme::MUTED), 0);

  s_barProgress = lv_bar_create(progRow);
  lv_obj_remove_style_all(s_barProgress);
  lv_obj_set_flex_grow(s_barProgress, 1);
  lv_obj_set_height(s_barProgress, 8);
  lv_bar_set_range(s_barProgress, 0, 1000);
  lv_bar_set_value(s_barProgress, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s_barProgress, lv_color_hex(StudioTheme::TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_barProgress, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_barProgress, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_barProgress, lv_color_hex(StudioTheme::CHAMPAGNE), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_barProgress, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(s_barProgress, 4, LV_PART_INDICATOR);

  s_lblTotTime = lv_label_create(progRow);
  lv_label_set_text(s_lblTotTime, "00:00");
  lv_obj_set_style_text_font(s_lblTotTime, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblTotTime, lv_color_hex(StudioTheme::MUTED), 0);

  // --- Bottom Controls Row (Backward, Pause/Start, Forward + Tombol Lirik di kanan bawah) ---
  lv_obj_t* ctrlRow = makePlainBox(s_pageLyrics, 0, 0, contentW, 56);
  lv_obj_set_layout(ctrlRow, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(ctrlRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ctrlRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Spacer kiri agar grup tombol kontrol tetap seimbang di tengah
  makePlainBox(ctrlRow, 0, 0, 56, 48);

  lv_obj_t* playbackGrp = makePlainBox(ctrlRow, 0, 0, LV_SIZE_CONTENT, 52);
  lv_obj_set_layout(playbackGrp, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(playbackGrp, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(playbackGrp, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(playbackGrp, 12, 0);

  makeCtrlBtn(playbackGrp, 52, 48, LV_SYMBOL_PREV, StudioTheme::RULE, StudioTheme::TEXT, onPrevClick);
  makeCtrlBtn(playbackGrp, 62, 50, LV_SYMBOL_PLAY, StudioTheme::CHAMPAGNE, StudioTheme::CHAMPAGNE,
              onToggleClick, &s_lblPlayPause);
  makeCtrlBtn(playbackGrp, 52, 48, LV_SYMBOL_NEXT, StudioTheme::RULE, StudioTheme::TEXT, onNextClick);

  // Tombol Lirik di kanan bawah
  s_btnLyrics = makeCtrlBtn(ctrlRow, 56, 48, LV_SYMBOL_LIST, StudioTheme::RULE, StudioTheme::MUTED,
                            onToggleLyricsViewClick, &s_lblLyricsBtn);

  xTaskCreatePinnedToCore(lyricsWsTask, "lyrics_ws", 6144, nullptr, 1, nullptr, 0);
  lv_timer_create(updateLyricsUiTimer, 100, nullptr);
  return s_pageLyrics;
}
