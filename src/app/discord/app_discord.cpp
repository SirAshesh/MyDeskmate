#include "app_discord.h"
#include "icon_discord.h"
#include "../../os/notification_shade.h"
#include "../../os/studio_theme.h"
#include "../../utils/text_sanitize.h"
#include "wifi_config.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#ifndef LYRICS_WS_HOST
#define LYRICS_WS_HOST "192.168.100.80"
#endif

#ifndef LYRICS_WS_PORT
#define LYRICS_WS_PORT 8765
#endif

namespace {
constexpr int MAX_DISCORD_MSGS   = 12;
constexpr int MAX_DISCORD_CHS    = 64;
constexpr int MAX_DISCORD_GUILDS = 100;

struct DiscordMsg {
  char id[28];
  char author[40];
  char timeWita[12];
  char content[224];
};

struct DiscordChannel {
  char id[28];
  char name[40];
};

lv_obj_t*   s_page         = nullptr;
lv_obj_t*   s_qrView       = nullptr;
lv_obj_t*   s_qrWidget     = nullptr;
lv_obj_t*   s_lblQrStatus  = nullptr;
lv_obj_t*   s_chatView     = nullptr;
lv_obj_t*   s_listBox      = nullptr;
lv_obj_t*   s_chListBox    = nullptr;
lv_obj_t*   s_lblChannel   = nullptr;
lv_obj_t*   s_badgeStatus  = nullptr;
lv_obj_t*   s_lblStatus    = nullptr;
lv_timer_t* s_uiTimer      = nullptr;

SemaphoreHandle_t s_mux         = nullptr;
bool              s_taskStarted = false;

enum class DiscordViewMode {
  CHAT = 0,
  GUILDS = 1,
  CHANNELS = 2
};

volatile bool s_dirty          = false;
volatile bool s_reqQrStart     = false;
volatile bool s_reqSync        = false;
volatile bool s_reqGuilds      = false;
volatile bool s_reqChannels    = false;
volatile bool s_reqLogout      = false;

bool            s_loggedIn      = false;
DiscordViewMode s_viewMode      = DiscordViewMode::CHAT;
char            s_authState[20] = "idle";
char            s_qrUrl[128]    = "";
char            s_lastRenderedQr[128] = "";
char            s_scannedUser[40]     = "";
char            s_authError[80]       = "";

DiscordChannel* s_guilds                   = nullptr;
int             s_guildCount               = 0;
char            s_activeGuildId[28]        = "dm";
char            s_activeGuildName[40]      = "Direct Messages";
char            s_renderedMsgKey[64]       = "";
char            s_renderedListKey[64]      = "";

DiscordChannel* s_channels                 = nullptr;
int             s_channelCount             = 0;
char            s_activeChId[28]           = "";
char            s_activeChName[40]         = "Pilih Channel";

DiscordMsg*     s_msgs                     = nullptr;
int             s_msgCount                 = 0;
char            s_statusText[24]           = "INIT";
uint32_t        s_statusColor              = StudioTheme::MUTED;
char            s_lastSeenId[28]           = "";
bool            s_hasFirstFetch            = false;
} // namespace

static void ensureDiscordBuffers() {
  if (!s_guilds) {
    s_guilds = (DiscordChannel*)heap_caps_calloc(MAX_DISCORD_GUILDS, sizeof(DiscordChannel), MALLOC_CAP_SPIRAM);
    if (!s_guilds) s_guilds = (DiscordChannel*)calloc(MAX_DISCORD_GUILDS, sizeof(DiscordChannel));
  }
  if (!s_channels) {
    s_channels = (DiscordChannel*)heap_caps_calloc(MAX_DISCORD_CHS, sizeof(DiscordChannel), MALLOC_CAP_SPIRAM);
    if (!s_channels) s_channels = (DiscordChannel*)calloc(MAX_DISCORD_CHS, sizeof(DiscordChannel));
  }
  if (!s_msgs) {
    s_msgs = (DiscordMsg*)heap_caps_calloc(MAX_DISCORD_MSGS, sizeof(DiscordMsg), MALLOC_CAP_SPIRAM);
    if (!s_msgs) s_msgs = (DiscordMsg*)calloc(MAX_DISCORD_MSGS, sizeof(DiscordMsg));
  }
}

static String baseUrl() {
  return String("http://") + LYRICS_WS_HOST + ":" + String(LYRICS_WS_PORT);
}

static bool extractJsonString(const String& json, const char* key, String& out, int startPos = 0) {
  String pat = String("\"") + key + "\"";
  int searchPos = startPos;
  while (true) {
    int idx = json.indexOf(pat, searchPos);
    if (idx < 0) return false;
    int p = idx + pat.length();
    while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
    if (p < (int)json.length() && json[p] == ':') {
      p++;
      while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
      if (p >= (int)json.length() || json[p] != '"') return false;
      p++;
      int s = p;
      bool esc = false;
      while (p < (int)json.length()) {
        char c = json[p];
        if (esc) {
          esc = false;
        } else if (c == '\\') {
          esc = true;
        } else if (c == '"') {
          out = json.substring(s, p);
          return true;
        }
        p++;
      }
      return false;
    }
    searchPos = idx + pat.length();
  }
}

static bool extractJsonBool(const String& json, const char* key, bool defVal = false) {
  String pat = String("\"") + key + "\"";
  int idx = json.indexOf(pat);
  if (idx < 0) return defVal;
  int colon = json.indexOf(':', idx + pat.length());
  if (colon < 0) return defVal;
  int p = colon + 1;
  while (p < (int)json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
  if (json.startsWith("true", p)) return true;
  if (json.startsWith("false", p)) return false;
  return defVal;
}

static void cleanCopy(const String& src, char* dst, size_t dstLen) {
  String s = src;
  s.replace("\\\"", "\"");
  s.replace("\\\\", "\\");
  s.replace("\\n", " ");
  sanitizeUtf8ForLvgl(s.c_str(), dst, dstLen);
}

static void parseStatusResponse(const String& body) {
  bool loggedIn = extractJsonBool(body, "loggedIn", false);
  String st, qr, su, err;
  extractJsonString(body, "state", st);
  extractJsonString(body, "qrUrl", qr);
  extractJsonString(body, "scannedUser", su);
  extractJsonString(body, "error", err);

  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
    bool wasLoggedIn = s_loggedIn;
    s_loggedIn = loggedIn;
    strlcpy(s_authState, st.c_str(), sizeof(s_authState));
    strlcpy(s_qrUrl, qr.c_str(), sizeof(s_qrUrl));
    cleanCopy(su, s_scannedUser, sizeof(s_scannedUser));
    cleanCopy(err, s_authError, sizeof(s_authError));
    if (loggedIn && !wasLoggedIn) {
      s_reqChannels = true;
      s_reqSync     = true;
    }
    s_dirty = true;
    xSemaphoreGive(s_mux);
  }
}

static void parseGuildsResponse(const String& body) {
  int arrStart = body.indexOf('[');
  if (arrStart < 0 || !s_guilds) return;

  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
    int count = 0;
    int pos = arrStart;
    while (count < MAX_DISCORD_GUILDS) {
      int ob = body.indexOf('{', pos);
      if (ob < 0) break;
      int oe = body.indexOf('}', ob);
      if (oe < 0) break;
      String obj = body.substring(ob, oe + 1);
      String id, name;
      if (extractJsonString(obj, "id", id) && extractJsonString(obj, "name", name)) {
        strlcpy(s_guilds[count].id, id.c_str(), sizeof(s_guilds[count].id));
        cleanCopy(name, s_guilds[count].name, sizeof(s_guilds[count].name));
        count++;
      }
      pos = oe + 1;
    }
    s_guildCount = count;
    s_dirty = true;
    xSemaphoreGive(s_mux);
  }
}

static void parseChannelsResponse(const String& body) {
  int arrStart = body.indexOf('[');
  if (arrStart < 0 || !s_channels) return;

  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
    int count = 0;
    int pos = arrStart;
    while (count < MAX_DISCORD_CHS) {
      int ob = body.indexOf('{', pos);
      if (ob < 0) break;
      int oe = body.indexOf('}', ob);
      if (oe < 0) break;
      String obj = body.substring(ob, oe + 1);
      String id, name;
      if (extractJsonString(obj, "id", id) && extractJsonString(obj, "name", name)) {
        strlcpy(s_channels[count].id, id.c_str(), sizeof(s_channels[count].id));
        cleanCopy(name, s_channels[count].name, sizeof(s_channels[count].name));
        count++;
      }
      pos = oe + 1;
    }
    s_channelCount = count;
    if (s_activeChId[0] == '\0' && count > 0) {
      strlcpy(s_activeChId, s_channels[0].id, sizeof(s_activeChId));
      strlcpy(s_activeChName, s_channels[0].name, sizeof(s_activeChName));
    }
    s_dirty = true;
    xSemaphoreGive(s_mux);
  }
}

static void parseMessagesResponse(const String& body) {
  if (!extractJsonBool(body, "ok", false)) {
    String err;
    extractJsonString(body, "error", err);
    if (err == "not_logged_in" || err == "token_expired") {
      if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_loggedIn = false;
        s_msgCount = 0;
        s_dirty = true;
        xSemaphoreGive(s_mux);
      }
      return;
    }
    if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
      snprintf(s_statusText, sizeof(s_statusText), "ERR");
      s_statusColor = StudioTheme::ROSE;
      s_dirty = true;
      xSemaphoreGive(s_mux);
    }
    return;
  }

  String chId, chName;
  extractJsonString(body, "channelId", chId);
  extractJsonString(body, "channelName", chName);

  int arrStart = body.indexOf("\"messages\"");
  arrStart = (arrStart >= 0) ? body.indexOf('[', arrStart) : -1;
  if (arrStart < 0) return;

  if (!s_msgs) return;
  int count = 0;
  int pos = arrStart;
  bool gotNew = false;
  char notifAuthor[40] = "";
  char notifBody[96] = "";
  char notifTime[12] = "";

  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
    while (count < MAX_DISCORD_MSGS) {
      int ob = body.indexOf('{', pos);
      if (ob < 0) break;
      int oe = body.indexOf('}', ob);
      if (oe < 0) break;
      String obj = body.substring(ob, oe + 1);
      String id, author, tm, content;
      extractJsonString(obj, "id", id);
      extractJsonString(obj, "author", author);
      extractJsonString(obj, "time", tm);
      extractJsonString(obj, "content", content);

      strlcpy(s_msgs[count].id, id.c_str(), sizeof(s_msgs[count].id));
      cleanCopy(author, s_msgs[count].author, sizeof(s_msgs[count].author));
      strlcpy(s_msgs[count].timeWita, tm.c_str(), sizeof(s_msgs[count].timeWita));
      cleanCopy(content, s_msgs[count].content, sizeof(s_msgs[count].content));
      count++;
      pos = oe + 1;
    }

    if (count > 0 && s_msgs[count - 1].id[0] != '\0') {
      if (s_hasFirstFetch && strcmp(s_msgs[count - 1].id, s_lastSeenId) != 0) {
        gotNew = true;
        strlcpy(notifAuthor, s_msgs[count - 1].author, sizeof(notifAuthor));
        strlcpy(notifBody, s_msgs[count - 1].content, sizeof(notifBody));
        strlcpy(notifTime, s_msgs[count - 1].timeWita, sizeof(notifTime));
      }
      strlcpy(s_lastSeenId, s_msgs[count - 1].id, sizeof(s_lastSeenId));
      s_hasFirstFetch = true;
    }

    s_msgCount = count;
    if (chId.length() > 0) strlcpy(s_activeChId, chId.c_str(), sizeof(s_activeChId));
    if (chName.length() > 0) cleanCopy(chName, s_activeChName, sizeof(s_activeChName));
    snprintf(s_statusText, sizeof(s_statusText), "LIVE (%d)", count);
    s_statusColor = StudioTheme::TEAL;
    s_dirty = true;
    xSemaphoreGive(s_mux);
  }

  if (gotNew) {
    char title[56];
    snprintf(title, sizeof(title), "Discord: %s", notifAuthor);
    notificationPush(title, notifBody, notifTime, StudioTheme::TEAL);
  }
}

static void discordBridgeTask(void* param) {
  (void)param;
  uint32_t lastPollMs = 0;
  bool statusChecked = false;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;
      http.setTimeout(7000);

      if (s_reqLogout) {
        s_reqLogout = false;
        if (http.begin(baseUrl() + "/discord/logout")) {
          http.POST("");
          http.end();
        }
        if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
          s_loggedIn = false;
          s_msgCount = 0;
          s_channelCount = 0;
          s_activeChId[0] = '\0';
          strlcpy(s_activeChName, "Pilih Channel", sizeof(s_activeChName));
          s_lastSeenId[0] = '\0';
          s_hasFirstFetch = false;
          s_reqQrStart = true;
          s_dirty = true;
          xSemaphoreGive(s_mux);
        }
      }

      if (!statusChecked) {
        statusChecked = true;
        if (http.begin(baseUrl() + "/discord/status")) {
          if (http.GET() == HTTP_CODE_OK) {
            parseStatusResponse(http.getString());
          }
          http.end();
        }
        if (!s_loggedIn && s_qrUrl[0] == '\0') {
          s_reqQrStart = true;
        }
      }

      if (s_reqQrStart) {
        s_reqQrStart = false;
        if (http.begin(baseUrl() + "/discord/qr/start")) {
          if (http.POST("") == HTTP_CODE_OK) {
            parseStatusResponse(http.getString());
          }
          http.end();
        }
      }

      uint32_t nowMs = millis();
      if (!s_loggedIn) {
        if (nowMs - lastPollMs >= 1500) {
          lastPollMs = nowMs;
          if (http.begin(baseUrl() + "/discord/status")) {
            if (http.GET() == HTTP_CODE_OK) {
              parseStatusResponse(http.getString());
            }
            http.end();
          }
        }
      } else {
        if (s_reqGuilds || s_guildCount == 0) {
          s_reqGuilds = false;
          if (http.begin(baseUrl() + "/discord/guilds")) {
            if (http.GET() == HTTP_CODE_OK) {
              parseGuildsResponse(http.getString());
            }
            http.end();
          }
        }

        if (s_reqChannels || s_channelCount == 0) {
          s_reqChannels = false;
          String chUrl = baseUrl() + "/discord/channels?guild_id=" + String(s_activeGuildId);
          if (http.begin(chUrl)) {
            if (http.GET() == HTTP_CODE_OK) {
              parseChannelsResponse(http.getString());
            }
            http.end();
          }
        }

        if (s_reqSync || (s_viewMode == DiscordViewMode::CHAT && (nowMs - lastPollMs >= 4000))) {
          s_reqSync = false;
          lastPollMs = nowMs;
          String url = baseUrl() + "/discord/messages?limit=12";
          if (s_activeChId[0] != '\0') {
            url += String("&channel_id=") + s_activeChId;
          }
          if (http.begin(url)) {
            if (http.GET() == HTTP_CODE_OK) {
              parseMessagesResponse(http.getString());
            }
            http.end();
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

static void onGuildSelectClick(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (idx >= 0 && idx < s_guildCount) {
      strlcpy(s_activeGuildId, s_guilds[idx].id, sizeof(s_activeGuildId));
      strlcpy(s_activeGuildName, s_guilds[idx].name, sizeof(s_activeGuildName));
      s_channelCount = 0;
      s_viewMode = DiscordViewMode::CHANNELS;
      s_reqChannels = true;
      s_dirty = true;
    }
    xSemaphoreGive(s_mux);
  }
}

static void onChannelSelectClick(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (idx >= 0 && idx < s_channelCount) {
      strlcpy(s_activeChId, s_channels[idx].id, sizeof(s_activeChId));
      strlcpy(s_activeChName, s_channels[idx].name, sizeof(s_activeChName));
      s_viewMode = DiscordViewMode::CHAT;
      s_hasFirstFetch = false;
      s_lastSeenId[0] = '\0';
      s_reqSync = true;
      s_dirty = true;
    }
    xSemaphoreGive(s_mux);
  }
}

static void renderDiscordUi() {
  if (!s_page) return;

  static char authState[20], qrUrl[128], scannedUser[40], authErr[80];
  static char activeChName[40], activeGuildName[40], statusText[24];
  bool loggedIn = false;
  DiscordViewMode viewMode = DiscordViewMode::CHAT;
  uint32_t statusColor = StudioTheme::MUTED;
  int msgCount = 0;
  int chCount = 0;
  int guildCount = 0;

  if (!s_mux || xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) != pdTRUE) return;

  loggedIn = s_loggedIn;
  viewMode = s_viewMode;
  strlcpy(authState, s_authState, sizeof(authState));
  strlcpy(qrUrl, s_qrUrl, sizeof(qrUrl));
  strlcpy(scannedUser, s_scannedUser, sizeof(s_scannedUser));
  strlcpy(authErr, s_authError, sizeof(authErr));
  strlcpy(activeChName, s_activeChName, sizeof(activeChName));
  strlcpy(activeGuildName, s_activeGuildName, sizeof(activeGuildName));
  strlcpy(statusText, s_statusText, sizeof(statusText));
  statusColor = s_statusColor;
  msgCount = s_msgCount;
  chCount = s_channelCount;
  guildCount = s_guildCount;
  s_dirty = false;
  xSemaphoreGive(s_mux);

  if (!loggedIn) {
    if (s_qrView) lv_obj_clear_flag(s_qrView, LV_OBJ_FLAG_HIDDEN);
    if (s_chatView) lv_obj_add_flag(s_chatView, LV_OBJ_FLAG_HIDDEN);

    if (s_qrWidget && qrUrl[0] != '\0' && strcmp(qrUrl, s_lastRenderedQr) != 0) {
      lv_qrcode_update(s_qrWidget, qrUrl, strlen(qrUrl));
      strlcpy(s_lastRenderedQr, qrUrl, sizeof(s_lastRenderedQr));
    }

    if (s_lblQrStatus) {
      if (strcmp(authState, "scanned") == 0) {
        lv_label_set_text_fmt(s_lblQrStatus, "Discan oleh %s!\nKonfirmasi login di aplikasi Discord HP.",
                              scannedUser[0] ? scannedUser : "User");
        lv_obj_set_style_text_color(s_lblQrStatus, lv_color_hex(StudioTheme::TEAL), 0);
      } else if (strcmp(authState, "qr_ready") == 0) {
        lv_label_set_text(s_lblQrStatus,
                          "Buka Discord di HP ->\nSettings -> Scan QR Code\nlalu arahkan ke kode ini.");
        lv_obj_set_style_text_color(s_lblQrStatus, lv_color_hex(StudioTheme::TEXT), 0);
      } else if (strcmp(authState, "expired") == 0) {
        lv_label_set_text(s_lblQrStatus, "QR Code kadaluarsa.\nTekan 'QR Baru' untuk mengulang.");
        lv_obj_set_style_text_color(s_lblQrStatus, lv_color_hex(StudioTheme::CORAL), 0);
      } else if (strcmp(authState, "error") == 0) {
        if (strstr(authErr, "400") != nullptr) {
          lv_label_set_text_fmt(s_lblQrStatus,
                                "Discord meminta Captcha.\nBuka http://%s:%d/discord\ndi browser sekali untuk login.",
                                LYRICS_WS_HOST, LYRICS_WS_PORT);
        } else {
          lv_label_set_text_fmt(s_lblQrStatus, "Error: %s", authErr[0] ? authErr : "Gagal");
        }
        lv_obj_set_style_text_color(s_lblQrStatus, lv_color_hex(StudioTheme::ROSE), 0);
      } else {
        lv_label_set_text(s_lblQrStatus, "Menghubungkan ke Discord Remote Auth...");
        lv_obj_set_style_text_color(s_lblQrStatus, lv_color_hex(StudioTheme::MUTED), 0);
      }
    }
    return;
  }

  // Logged in -> tampilkan Chat View
  if (s_qrView) lv_obj_add_flag(s_qrView, LV_OBJ_FLAG_HIDDEN);
  if (s_chatView) lv_obj_clear_flag(s_chatView, LV_OBJ_FLAG_HIDDEN);

  if (s_lblChannel) {
    if (viewMode == DiscordViewMode::GUILDS)        lv_label_set_text(s_lblChannel, "Pilih Server / DM");
    else if (viewMode == DiscordViewMode::CHANNELS) lv_label_set_text(s_lblChannel, activeGuildName);
    else                                            lv_label_set_text(s_lblChannel, activeChName);
  }
  if (s_badgeStatus) {
    lv_obj_set_style_bg_color(s_badgeStatus, lv_color_hex(statusColor), 0);
  }

  DiscordChannel* guilds = s_guilds;
  DiscordChannel* chs    = s_channels;
  DiscordMsg*     msgs   = s_msgs;

  if (viewMode == DiscordViewMode::GUILDS && guilds) {
    lv_obj_add_flag(s_listBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_chListBox, LV_OBJ_FLAG_HIDDEN);

    char listKey[64];
    snprintf(listKey, sizeof(listKey), "G:%d:%s", guildCount, s_activeGuildId);
    if (strcmp(listKey, s_renderedListKey) == 0 && lv_obj_get_child_cnt(s_chListBox) > 0) {
      return;
    }
    strlcpy(s_renderedListKey, listKey, sizeof(s_renderedListKey));
    lv_obj_clean(s_chListBox);

    for (int i = 0; i < guildCount; i++) {
      lv_obj_t* btn = lv_btn_create(s_chListBox);
      lv_obj_remove_style_all(btn);
      lv_obj_set_width(btn, lv_pct(100));
      lv_obj_set_height(btn, 36);
      bool isSel = (strcmp(guilds[i].id, s_activeGuildId) == 0);
      lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::SURFACE), 0);
      lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::ELEVATED), LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(btn, lv_color_hex(isSel ? StudioTheme::CHAMPAGNE : StudioTheme::RULE), 0);
      lv_obj_set_style_border_width(btn, 1, 0);
      lv_obj_set_style_radius(btn, 8, 0);
      lv_obj_set_style_pad_hor(btn, 10, 0);
      lv_obj_add_event_cb(btn, onGuildSelectClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);

      lv_obj_t* lbl = lv_label_create(btn);
      lv_label_set_text(lbl, guilds[i].name);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(isSel ? StudioTheme::CHAMPAGNE : StudioTheme::TEXT), 0);
      lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
    return;
  }

  if (viewMode == DiscordViewMode::CHANNELS && chs) {
    lv_obj_add_flag(s_listBox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_chListBox, LV_OBJ_FLAG_HIDDEN);

    char listKey[64];
    snprintf(listKey, sizeof(listKey), "C:%s:%d:%s", s_activeGuildId, chCount, s_activeChId);
    if (strcmp(listKey, s_renderedListKey) == 0 && lv_obj_get_child_cnt(s_chListBox) > 0) {
      return;
    }
    strlcpy(s_renderedListKey, listKey, sizeof(s_renderedListKey));
    lv_obj_clean(s_chListBox);

    for (int i = 0; i < chCount; i++) {
      lv_obj_t* btn = lv_btn_create(s_chListBox);
      lv_obj_remove_style_all(btn);
      lv_obj_set_width(btn, lv_pct(100));
      lv_obj_set_height(btn, 36);
      bool isSel = (strcmp(chs[i].id, s_activeChId) == 0);
      lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::SURFACE), 0);
      lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::ELEVATED), LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(btn, lv_color_hex(isSel ? StudioTheme::TEAL : StudioTheme::RULE), 0);
      lv_obj_set_style_border_width(btn, 1, 0);
      lv_obj_set_style_radius(btn, 8, 0);
      lv_obj_set_style_pad_hor(btn, 10, 0);
      lv_obj_add_event_cb(btn, onChannelSelectClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);

      lv_obj_t* lbl = lv_label_create(btn);
      lv_label_set_text(lbl, chs[i].name);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(isSel ? StudioTheme::TEAL : StudioTheme::TEXT), 0);
      lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
    return;
  }

  lv_obj_add_flag(s_chListBox, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_listBox, LV_OBJ_FLAG_HIDDEN);

  char newKey[64];
  snprintf(newKey, sizeof(newKey), "%s:%d:%s", s_activeChId, msgCount,
           msgCount > 0 ? msgs[msgCount - 1].id : "");
  if (strcmp(newKey, s_renderedMsgKey) == 0 && lv_obj_get_child_cnt(s_listBox) > 0) {
    return;
  }
  strlcpy(s_renderedMsgKey, newKey, sizeof(s_renderedMsgKey));

  bool wasAtBottom = (lv_obj_get_child_cnt(s_listBox) == 0) ||
                     (lv_obj_get_scroll_bottom(s_listBox) <= 24);
  lv_coord_t prevScrollY = lv_obj_get_scroll_y(s_listBox);
  lv_obj_clean(s_listBox);

  if (msgCount == 0) {
    lv_obj_t* lbl = lv_label_create(s_listBox);
    lv_label_set_text(lbl, "Memuat pesan...");
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(StudioTheme::MUTED), 0);
    return;
  }

  for (int i = 0; i < msgCount; i++) {
    lv_obj_t* card = lv_obj_create(s_listBox);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(StudioTheme::SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(StudioTheme::RULE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_hor(card, 10, 0);
    lv_obj_set_style_pad_ver(card, 7, 0);
    lv_obj_set_style_pad_row(card, 3, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* topRow = makePlainBox(card, 0, 0, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(topRow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(topRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* lblAuthor = lv_label_create(topRow);
    lv_label_set_text(lblAuthor, msgs[i].author);
    lv_obj_set_style_text_font(lblAuthor, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lblAuthor, lv_color_hex(StudioTheme::CHAMPAGNE), 0);

    lv_obj_t* lblTime = lv_label_create(topRow);
    lv_label_set_text(lblTime, msgs[i].timeWita);
    lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lblTime, lv_color_hex(StudioTheme::SUBTLE), 0);

    lv_obj_t* lblBody = lv_label_create(card);
    lv_obj_set_width(lblBody, lv_pct(100));
    lv_label_set_long_mode(lblBody, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lblBody, msgs[i].content);
    lv_obj_set_style_text_font(lblBody, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lblBody, lv_color_hex(StudioTheme::TEXT), 0);
  }

  lv_obj_update_layout(s_listBox);
  if (wasAtBottom) {
    lv_obj_scroll_to_y(s_listBox, LV_COORD_MAX, LV_ANIM_OFF);
  } else {
    lv_obj_scroll_to_y(s_listBox, prevScrollY, LV_ANIM_OFF);
  }
}

static void onDiscordUiTimer(lv_timer_t* timer) {
  (void)timer;
  if (s_dirty) renderDiscordUi();
}

static void onNewQrClick(lv_event_t* e) {
  (void)e;
  s_lastRenderedQr[0] = '\0';
  s_renderedMsgKey[0] = '\0';
  s_renderedListKey[0] = '\0';
  s_reqQrStart = true;
}

static void onToggleGuildsClick(lv_event_t* e) {
  (void)e;
  s_viewMode = (s_viewMode == DiscordViewMode::GUILDS) ? DiscordViewMode::CHAT : DiscordViewMode::GUILDS;
  if (s_viewMode == DiscordViewMode::GUILDS) s_reqGuilds = true;
  s_dirty = true;
}

static void onToggleChannelsClick(lv_event_t* e) {
  (void)e;
  s_viewMode = (s_viewMode == DiscordViewMode::CHANNELS) ? DiscordViewMode::CHAT : DiscordViewMode::CHANNELS;
  if (s_viewMode == DiscordViewMode::CHANNELS) s_reqChannels = true;
  s_dirty = true;
}

static void onSyncClick(lv_event_t* e) {
  (void)e;
  s_reqSync = true;
}

static void onLogoutClick(lv_event_t* e) {
  (void)e;
  s_reqLogout = true;
}

static lv_obj_t* makeMiniBtn(lv_obj_t* parent, int w, int h, const char* txt, uint32_t col, lv_event_cb_t cb) {
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(col), 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lbl = lv_label_create(btn);
  lv_label_set_text(lbl, txt);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(col), 0);
  lv_obj_center(lbl);
  return btn;
}

static lv_obj_t* discordOnCreate(lv_obj_t* parent, int rightW, int rightH) {
  ensureDiscordBuffers();
  if (!s_mux) {
    s_mux = xSemaphoreCreateMutex();
  }
  s_lastRenderedQr[0] = '\0';
  s_renderedMsgKey[0] = '\0';
  s_renderedListKey[0] = '\0';

  s_page = makePlainBox(parent, 0, 0, rightW, rightH);
  const int contentW = rightW - 28;

  // --- VIEW 1: QR CODE LOGIN ---
  s_qrView = makePlainBox(s_page, 0, 0, rightW, rightH);
  lv_obj_set_style_pad_all(s_qrView, 16, 0);
  lv_obj_set_layout(s_qrView, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_qrView, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_qrView, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s_qrView, 18, 0);

  lv_obj_t* qrCard = lv_obj_create(s_qrView);
  lv_obj_remove_style_all(qrCard);
  lv_obj_set_size(qrCard, 174, 174);
  lv_obj_set_style_bg_color(qrCard, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(qrCard, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(qrCard, 12, 0);

  s_qrWidget = lv_qrcode_create(qrCard, 152, lv_color_black(), lv_color_white());
  lv_obj_center(s_qrWidget);

  lv_obj_t* qrInfo = makePlainBox(s_qrView, 0, 0, rightW - 32 - 174 - 18, LV_SIZE_CONTENT);
  lv_obj_set_layout(qrInfo, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(qrInfo, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(qrInfo, 10, 0);

  lv_obj_t* lblQrTitle = lv_label_create(qrInfo);
  lv_label_set_text(lblQrTitle, "Login Discord");
  lv_obj_set_style_text_font(lblQrTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblQrTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_lblQrStatus = lv_label_create(qrInfo);
  lv_obj_set_width(s_lblQrStatus, lv_pct(100));
  lv_label_set_long_mode(s_lblQrStatus, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(s_lblQrStatus, &lv_font_montserrat_12, 0);

  makeMiniBtn(qrInfo, 110, 32, "QR Baru", StudioTheme::TEAL, onNewQrClick);

  // --- VIEW 2: CHAT & CHANNEL READER ---
  s_chatView = makePlainBox(s_page, 0, 0, rightW, rightH);
  lv_obj_set_style_pad_hor(s_chatView, 14, 0);
  lv_obj_set_style_pad_ver(s_chatView, 10, 0);
  lv_obj_set_style_pad_row(s_chatView, 8, 0);
  lv_obj_set_layout(s_chatView, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_chatView, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* header = makePlainBox(s_chatView, 0, 0, contentW, 32);
  lv_obj_set_layout(header, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(header, 8, 0);

  s_badgeStatus = makePlainBox(header, 0, 0, 8, 8);
  lv_obj_set_style_bg_color(s_badgeStatus, lv_color_hex(s_statusColor), 0);
  lv_obj_set_style_bg_opa(s_badgeStatus, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_badgeStatus, LV_RADIUS_CIRCLE, 0);

  s_lblChannel = lv_label_create(header);
  lv_label_set_text(s_lblChannel, s_activeChName);
  lv_obj_set_flex_grow(s_lblChannel, 1);
  lv_label_set_long_mode(s_lblChannel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_lblChannel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_lblChannel, lv_color_hex(StudioTheme::TEXT), 0);

  lv_obj_t* rightGrp = makePlainBox(header, 0, 0, 130, 30);
  lv_obj_set_layout(rightGrp, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(rightGrp, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rightGrp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(rightGrp, 6, 0);

  makeMiniBtn(rightGrp, 28, 26, LV_SYMBOL_DIRECTORY, StudioTheme::CHAMPAGNE, onToggleGuildsClick);
  makeMiniBtn(rightGrp, 28, 26, LV_SYMBOL_LIST, StudioTheme::CHAMPAGNE, onToggleChannelsClick);
  makeMiniBtn(rightGrp, 28, 26, LV_SYMBOL_REFRESH, StudioTheme::TEAL, onSyncClick);
  makeMiniBtn(rightGrp, 28, 26, LV_SYMBOL_POWER, StudioTheme::ROSE, onLogoutClick);

  const int listH = rightH - 20 - 32 - 8;
  s_listBox = makePlainBox(s_chatView, 0, 0, contentW, listH);
  lv_obj_add_flag(s_listBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s_listBox, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_listBox, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_layout(s_listBox, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_listBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_listBox, 6, 0);

  s_chListBox = makePlainBox(s_chatView, 0, 0, contentW, listH);
  lv_obj_add_flag(s_chListBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s_chListBox, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_chListBox, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_layout(s_chListBox, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_chListBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_chListBox, 6, 0);
  lv_obj_add_flag(s_chListBox, LV_OBJ_FLAG_HIDDEN);

  if (!s_taskStarted) {
    s_taskStarted = true;
    xTaskCreatePinnedToCore(discordBridgeTask, "discord_qr", 8192, nullptr, 1, nullptr, 0);
  }

  s_dirty = true;
  renderDiscordUi();
  s_uiTimer = lv_timer_create(onDiscordUiTimer, 250, nullptr);
  return s_page;
}

static void discordOnDestroy() {
  if (s_uiTimer) {
    lv_timer_del(s_uiTimer);
    s_uiTimer = nullptr;
  }
  s_page        = nullptr;
  s_qrView      = nullptr;
  s_qrWidget    = nullptr;
  s_lblQrStatus = nullptr;
  s_chatView    = nullptr;
  s_listBox     = nullptr;
  s_chListBox   = nullptr;
  s_lblChannel  = nullptr;
  s_badgeStatus = nullptr;
  s_lblStatus   = nullptr;
}

AppDescriptor appDiscordDescriptor() {
  return AppDescriptor{
    "discord",
    "Discord",
    nullptr,
    &icon_discord,
    false,
    discordOnCreate,
    discordOnDestroy,
    nullptr,
    nullptr,
    nullptr
  };
}
