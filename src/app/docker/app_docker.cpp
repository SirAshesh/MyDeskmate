#include "app_docker.h"
#include "icon_docker.h"
#include "../../os/notification_shade.h"
#include "../../os/studio_theme.h"
#include "../../utils/text_sanitize.h"
#include "wifi_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
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
constexpr int    MAX_CONTAINERS = 32;
constexpr size_t LOG_BUF_SIZE   = 2048;
constexpr size_t RX_BUF_SIZE    = 8192;

struct DockerContainer {
  char id[16];
  char name[32];
  char state[16];
  char status[40];
};

enum class DockerViewMode : uint8_t {
  LIST = 0,
  LOGS = 1,
};

lv_obj_t*   s_page        = nullptr;
lv_obj_t*   s_dotStatus   = nullptr;
lv_obj_t*   s_lblTitle    = nullptr;
lv_obj_t*   s_btnBack     = nullptr;
lv_obj_t*   s_btnRestart  = nullptr;
lv_obj_t*   s_listBox     = nullptr;
lv_obj_t*   s_logPanel    = nullptr;
lv_obj_t*   s_lblStats    = nullptr;
lv_obj_t*   s_logScroll   = nullptr;
lv_obj_t*   s_lblLogs     = nullptr;
lv_timer_t* s_uiTimer     = nullptr;

SemaphoreHandle_t s_mux           = nullptr;
bool              s_taskStarted   = false;
volatile bool     s_dirty         = false;
volatile bool     s_reqListSync   = false;
volatile bool     s_reqLogSync    = false;
volatile bool     s_reqRestart    = false;
volatile bool     s_uiOpen        = false;

DockerContainer* s_containers     = nullptr;
DockerContainer* s_snapList       = nullptr;
char*            s_logText        = nullptr;
char*            s_snapLogs       = nullptr;
char*            s_rxBuf          = nullptr;
int              s_count          = 0;
int              s_runningCount   = 0;
int              s_totalCount     = 0;
int              s_contentW       = 383;
uint32_t         s_listSeq        = 0;
uint32_t         s_logSeq         = 0;
DockerViewMode   s_viewMode       = DockerViewMode::LIST;
char             s_selId[16]      = "";
char             s_selName[32]    = "";
char             s_selStats[80]   = "Memuat statistik...";
char             s_errMsg[128]    = "Menghubungkan ke Docker bridge...";
uint32_t         s_statusColor    = StudioTheme::MUTED;

uint32_t s_renderedListSeq        = 0xFFFFFFFFUL;
uint32_t s_renderedLogSeq         = 0xFFFFFFFFUL;

bool     s_alertInitDone          = false;
uint32_t s_prevRunningHashes[MAX_CONTAINERS] = {};
int      s_prevRunningCount       = 0;
char     s_pendingDownName[32]    = "";
} // namespace

static void ensureDockerBuffers() {
  if (!s_containers) {
    s_containers = (DockerContainer*)heap_caps_calloc(MAX_CONTAINERS, sizeof(DockerContainer), MALLOC_CAP_SPIRAM);
    if (!s_containers) s_containers = (DockerContainer*)calloc(MAX_CONTAINERS, sizeof(DockerContainer));
  }
  if (!s_snapList) {
    s_snapList = (DockerContainer*)heap_caps_calloc(MAX_CONTAINERS, sizeof(DockerContainer), MALLOC_CAP_SPIRAM);
    if (!s_snapList) s_snapList = (DockerContainer*)calloc(MAX_CONTAINERS, sizeof(DockerContainer));
  }
  if (!s_logText) {
    s_logText = (char*)heap_caps_calloc(1, LOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_logText) s_logText = (char*)calloc(1, LOG_BUF_SIZE);
  }
  if (!s_snapLogs) {
    s_snapLogs = (char*)heap_caps_calloc(1, LOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_snapLogs) s_snapLogs = (char*)calloc(1, LOG_BUF_SIZE);
  }
  if (!s_rxBuf) {
    s_rxBuf = (char*)heap_caps_calloc(1, RX_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_rxBuf) s_rxBuf = (char*)calloc(1, RX_BUF_SIZE);
  }
}

static uint32_t hashStr(const char* s) {
  uint32_t h = 2166136261UL;
  while (s && *s) {
    h ^= (uint8_t)(*s++);
    h *= 16777619UL;
  }
  return h;
}

static const char* findJsonKey(const char* start, const char* end, const char* key) {
  size_t klen = strlen(key);
  const char* p = start;
  while (p && p < end) {
    const char* q = strchr(p, '"');
    if (!q || q >= end) return nullptr;
    q++;
    if ((size_t)(end - q) > klen + 1 && strncmp(q, key, klen) == 0 && q[klen] == '"') {
      const char* after = q + klen + 1;
      while (after < end && (*after == ' ' || *after == '\t')) after++;
      if (after < end && *after == ':') {
        after++;
        while (after < end && (*after == ' ' || *after == '\t')) after++;
        return after;
      }
    }
    p = q;
  }
  return nullptr;
}

static bool extractJsonStrRange(const char* start, const char* end, const char* key,
                                char* dst, size_t dstLen, bool preserveNewline = false) {
  if (!dst || dstLen == 0) return false;
  dst[0] = '\0';
  const char* val = findJsonKey(start, end, key);
  if (!val || val >= end || *val != '"') return false;
  val++;

  size_t out = 0;
  bool esc = false;
  while (val < end && *val != '\0') {
    char c = *val++;
    if (esc) {
      esc = false;
      if (out + 1 < dstLen) {
        if (c == 'n') dst[out++] = preserveNewline ? '\n' : ' ';
        else if (c == 'r' || c == 't') dst[out++] = ' ';
        else dst[out++] = c;
      }
      continue;
    }
    if (c == '\\') {
      esc = true;
      continue;
    }
    if (c == '"') {
      dst[out] = '\0';
      sanitizeUtf8ForLvgl(dst, dst, dstLen);
      return true;
    }
    if (out + 1 < dstLen) {
      dst[out++] = c;
    }
  }
  dst[out] = '\0';
  return false;
}

static int extractJsonIntRange(const char* start, const char* end, const char* key, int defVal = 0) {
  const char* val = findJsonKey(start, end, key);
  if (!val || val >= end) return defVal;
  return atoi(val);
}

static float extractJsonFloatRange(const char* start, const char* end, const char* key, float defVal = 0.0f) {
  const char* val = findJsonKey(start, end, key);
  if (!val || val >= end) return defVal;
  return (float)atof(val);
}

static bool extractJsonBoolRange(const char* start, const char* end, const char* key, bool defVal = false) {
  const char* val = findJsonKey(start, end, key);
  if (!val || val >= end) return defVal;
  if (strncmp(val, "true", 4) == 0) return true;
  if (strncmp(val, "false", 5) == 0) return false;
  return defVal;
}

// Direct WiFiClient HTTP/1.0 call -> baca response body langsung ke PSRAM s_rxBuf
static int httpFetchToRxBuf(const char* method, const char* path, uint32_t timeoutMs, const char*& outBody, size_t& outBodyLen) {
  outBody = nullptr;
  outBodyLen = 0;
  if (!s_rxBuf) return -10;

  WiFiClient client;
  client.setTimeout(timeoutMs / 1000 + 1);
  if (!client.connect(LYRICS_WS_HOST, LYRICS_WS_PORT, (int32_t)4000)) {
    return -1;
  }

  client.printf("%s %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n",
                method, path, LYRICS_WS_HOST, LYRICS_WS_PORT);
  if (strcmp(method, "POST") == 0) {
    client.print("Content-Length: 0\r\n");
  }
  client.print("\r\n");

  uint32_t startMs = millis();
  size_t totalRead = 0;
  while (millis() - startMs < timeoutMs) {
    while (client.available() > 0 && totalRead + 1 < RX_BUF_SIZE) {
      int n = client.read((uint8_t*)(s_rxBuf + totalRead), RX_BUF_SIZE - 1 - totalRead);
      if (n > 0) {
        totalRead += (size_t)n;
        startMs = millis();
      } else {
        break;
      }
    }
    if (!client.connected() && client.available() == 0) break;
    if (totalRead + 1 >= RX_BUF_SIZE) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  client.stop();
  s_rxBuf[totalRead] = '\0';

  if (totalRead == 0) return -2;

  int httpStatus = 0;
  const char* sp = strchr(s_rxBuf, ' ');
  if (sp) httpStatus = atoi(sp + 1);

  const char* hdrEnd = strstr(s_rxBuf, "\r\n\r\n");
  if (hdrEnd) {
    outBody = hdrEnd + 4;
  } else {
    hdrEnd = strstr(s_rxBuf, "\n\n");
    outBody = hdrEnd ? (hdrEnd + 2) : s_rxBuf;
  }
  outBodyLen = totalRead - (size_t)(outBody - s_rxBuf);
  return httpStatus > 0 ? httpStatus : -3;
}

static void setListErrorLocked(const char* msg, uint32_t col = StudioTheme::ROSE) {
  if (strcmp(s_errMsg, msg) != 0 || s_statusColor != col || s_count != 0) {
    strlcpy(s_errMsg, msg, sizeof(s_errMsg));
    s_statusColor = col;
    s_listSeq++;
    s_dirty = true;
  }
}

static void parseContainersBuf(const char* body, size_t bodyLen) {
  const char* end = body + bodyLen;
  if (!extractJsonBoolRange(body, end, "ok", false) || !s_containers) {
    char errStr[64] = "";
    extractJsonStrRange(body, end, "error", errStr, sizeof(errStr));
    char msg[128];
    if (errStr[0] != '\0') snprintf(msg, sizeof(msg), "Bridge error: %s", errStr);
    else                    snprintf(msg, sizeof(msg), "Respons Docker invalid (len %u)", (unsigned)bodyLen);
    if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_count = 0;
      setListErrorLocked(msg, StudioTheme::ROSE);
      xSemaphoreGive(s_mux);
    }
    return;
  }

  int running = extractJsonIntRange(body, end, "running", 0);
  int total   = extractJsonIntRange(body, end, "total", 0);
  const char* arrKey = findJsonKey(body, end, "containers");
  const char* arrStart = (arrKey && arrKey < end) ? strchr(arrKey, '[') : nullptr;
  if (!arrStart || arrStart >= end) {
    if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_count = 0;
      setListErrorLocked("Field containers tidak ditemukan", StudioTheme::ROSE);
      xSemaphoreGive(s_mux);
    }
    return;
  }

  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
    int count = 0;
    const char* pos = arrStart + 1;
    uint32_t curRunningHashes[MAX_CONTAINERS] = {};
    int curRunningCnt = 0;

    while (count < MAX_CONTAINERS && pos < end) {
      const char* ob = strchr(pos, '{');
      if (!ob || ob >= end) break;
      const char* oe = strchr(ob, '}');
      if (!oe || oe >= end) break;

      if (extractJsonStrRange(ob, oe + 1, "id", s_containers[count].id, sizeof(s_containers[count].id)) &&
          extractJsonStrRange(ob, oe + 1, "name", s_containers[count].name, sizeof(s_containers[count].name))) {
        extractJsonStrRange(ob, oe + 1, "state", s_containers[count].state, sizeof(s_containers[count].state));
        extractJsonStrRange(ob, oe + 1, "status", s_containers[count].status, sizeof(s_containers[count].status));

        if (strcmp(s_containers[count].state, "running") == 0) {
          if (curRunningCnt < MAX_CONTAINERS) {
            curRunningHashes[curRunningCnt++] = hashStr(s_containers[count].id);
          }
        } else if (s_alertInitDone && s_pendingDownName[0] == '\0') {
          uint32_t h = hashStr(s_containers[count].id);
          for (int p = 0; p < s_prevRunningCount; p++) {
            if (s_prevRunningHashes[p] == h) {
              strlcpy(s_pendingDownName, s_containers[count].name, sizeof(s_pendingDownName));
              break;
            }
          }
        }
        count++;
      }
      pos = oe + 1;
    }

    memcpy(s_prevRunningHashes, curRunningHashes, sizeof(curRunningHashes));
    s_prevRunningCount = curRunningCnt;
    s_alertInitDone    = true;

    s_count        = count;
    s_runningCount = running;
    s_totalCount   = total;
    s_statusColor  = (total > 0 && running == total) ? StudioTheme::TEAL : StudioTheme::CORAL;
    if (count == 0) strlcpy(s_errMsg, "Tidak ada container di host", sizeof(s_errMsg));
    else            s_errMsg[0] = '\0';
    s_listSeq++;
    s_dirty = true;
    xSemaphoreGive(s_mux);
  }
}

static void parseLogsBuf(const char* body, size_t bodyLen) {
  const char* end = body + bodyLen;
  if (!extractJsonBoolRange(body, end, "ok", false) || !s_logText) {
    char errStr[80] = "";
    extractJsonStrRange(body, end, "error", errStr, sizeof(errStr));
    if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
      snprintf(s_selStats, sizeof(s_selStats), "Gagal memuat log");
      snprintf(s_logText, LOG_BUF_SIZE, "%s", errStr[0] ? errStr : "Bridge error / log kosong");
      s_statusColor = StudioTheme::ROSE;
      s_logSeq++;
      s_dirty = true;
      xSemaphoreGive(s_mux);
    }
    return;
  }

  char statusStr[40] = "";
  extractJsonStrRange(body, end, "status", statusStr, sizeof(statusStr));
  float cpuPct = extractJsonFloatRange(body, end, "cpuPct", 0.0f);
  int   memMb  = extractJsonIntRange(body, end, "memMb", 0);

  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
    snprintf(s_selStats, sizeof(s_selStats), "%s  |  CPU: %.1f%%  |  RAM: %d MB",
             statusStr, cpuPct, memMb);
    extractJsonStrRange(body, end, "logs", s_logText, LOG_BUF_SIZE, true);
    s_statusColor = StudioTheme::TEAL;
    s_logSeq++;
    s_dirty = true;
    xSemaphoreGive(s_mux);
  }
}

static uint32_t stateColor(const char* state, const char* status) {
  if (strcmp(state, "running") == 0) {
    if (strstr(status, "unhealthy") != nullptr) return StudioTheme::CORAL;
    return StudioTheme::SAGE;
  }
  if (strcmp(state, "restarting") == 0 || strcmp(state, "paused") == 0) {
    return StudioTheme::CHAMPAGNE;
  }
  return StudioTheme::ROSE;
}

static void dockerPollTask(void* param) {
  (void)param;
  uint32_t lastListMs = 0;
  uint32_t lastLogMs  = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_uiOpen && s_count == 0) {
          setListErrorLocked("WiFi belum terhubung", StudioTheme::ROSE);
        }
        xSemaphoreGive(s_mux);
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    uint32_t nowMs = millis();

    bool doRestart = false;
    bool uiOpen = false;
    DockerViewMode mode = DockerViewMode::LIST;
    char selId[16] = "";
    bool reqList = false;
    bool reqLog  = false;

    if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
      uiOpen = s_uiOpen;
      mode   = s_viewMode;
      strlcpy(selId, s_selId, sizeof(selId));
      if (s_reqRestart && selId[0] != '\0') {
        s_reqRestart = false;
        doRestart = true;
      }
      if (s_reqListSync) {
        s_reqListSync = false;
        reqList = true;
      }
      if (s_reqLogSync) {
        s_reqLogSync = false;
        reqLog = true;
      }
      xSemaphoreGive(s_mux);
    }

    if (doRestart) {
      char path[48];
      snprintf(path, sizeof(path), "/docker/restart?id=%s", selId);
      const char* body = nullptr;
      size_t blen = 0;
      httpFetchToRxBuf("POST", path, 15000, body, blen);
      reqLog  = (mode == DockerViewMode::LOGS);
      reqList = true;
    }

    bool listDue = reqList ||
                   (uiOpen && mode == DockerViewMode::LIST && (nowMs - lastListMs >= 5000 || lastListMs == 0)) ||
                   (!uiOpen && (nowMs - lastListMs >= 20000 || lastListMs == 0));

    if (listDue && (mode == DockerViewMode::LIST || !uiOpen || reqList)) {
      lastListMs = nowMs;
      const char* body = nullptr;
      size_t blen = 0;
      int code = httpFetchToRxBuf("GET", "/docker/containers", 6000, body, blen);
      if (code == 200 && body && blen > 0) {
        parseContainersBuf(body, blen);
      } else {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Gagal akses %s:%d (err %d)\nIP: %s  |  Heap: %uK",
                 LYRICS_WS_HOST, LYRICS_WS_PORT, code,
                 WiFi.localIP().toString().c_str(), (unsigned)(ESP.getFreeHeap() / 1024));
        if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
          setListErrorLocked(msg, StudioTheme::ROSE);
          xSemaphoreGive(s_mux);
        }
      }
    }

    bool logDue = uiOpen && mode == DockerViewMode::LOGS && selId[0] != '\0' &&
                  (reqLog || (nowMs - lastLogMs >= 3000 || lastLogMs == 0));
    if (logDue) {
      lastLogMs = nowMs;
      char path[64];
      snprintf(path, sizeof(path), "/docker/logs?id=%s&tail=18", selId);
      const char* body = nullptr;
      size_t blen = 0;
      int code = httpFetchToRxBuf("GET", path, 8000, body, blen);
      if (code == 200 && body && blen > 0) {
        parseLogsBuf(body, blen);
      } else if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(s_selStats, sizeof(s_selStats), "Log gagal (err %d)", code);
        if (s_logText) {
          snprintf(s_logText, LOG_BUF_SIZE, "Gagal mengambil log dari %s:%d (err %d)",
                   LYRICS_WS_HOST, LYRICS_WS_PORT, code);
        }
        s_statusColor = StudioTheme::ROSE;
        s_logSeq++;
        s_dirty = true;
        xSemaphoreGive(s_mux);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

static void flushPendingNotification() {
  char downName[32] = "";
  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (s_pendingDownName[0] != '\0') {
      strlcpy(downName, s_pendingDownName, sizeof(downName));
      s_pendingDownName[0] = '\0';
    }
    xSemaphoreGive(s_mux);
  }
  if (downName[0] != '\0') {
    char msg[64];
    snprintf(msg, sizeof(msg), "Container '%s' tidak aktif", downName);
    notificationPush("Docker Alert", msg, nullptr, StudioTheme::ROSE);
  }
}

static void onContainerClick(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (!s_containers || !s_mux || xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (idx >= 0 && idx < s_count) {
    strlcpy(s_selId, s_containers[idx].id, sizeof(s_selId));
    strlcpy(s_selName, s_containers[idx].name, sizeof(s_selName));
    strlcpy(s_selStats, "Memuat log & statistik...", sizeof(s_selStats));
    if (s_logText) s_logText[0] = '\0';
    s_viewMode       = DockerViewMode::LOGS;
    s_renderedLogSeq = 0xFFFFFFFFUL;
    s_logSeq++;
    s_reqLogSync     = true;
    s_dirty          = true;
  }
  xSemaphoreGive(s_mux);
}

static void onBackClick(lv_event_t* e) {
  (void)e;
  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_viewMode        = DockerViewMode::LIST;
    s_renderedListSeq = 0xFFFFFFFFUL;
    s_reqListSync     = true;
    s_dirty           = true;
    xSemaphoreGive(s_mux);
  }
}

static void onRefreshClick(lv_event_t* e) {
  (void)e;
  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_statusColor = StudioTheme::CHAMPAGNE;
    if (s_viewMode == DockerViewMode::LOGS) s_reqLogSync  = true;
    else                                    s_reqListSync = true;
    s_dirty = true;
    xSemaphoreGive(s_mux);
  }
}

static void onRestartClick(lv_event_t* e) {
  (void)e;
  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
    strlcpy(s_selStats, "Restarting container...", sizeof(s_selStats));
    s_statusColor = StudioTheme::CHAMPAGNE;
    s_logSeq++;
    s_reqRestart  = true;
    s_dirty       = true;
    xSemaphoreGive(s_mux);
  }
}

static void renderDockerUi() {
  if (!s_page || !s_mux || xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) != pdTRUE) return;

  DockerViewMode mode    = s_viewMode;
  int            count   = s_count;
  int            running = s_runningCount;
  int            total   = s_totalCount;
  uint32_t       listSeq = s_listSeq;
  uint32_t       logSeq  = s_logSeq;
  uint32_t       stCol   = s_statusColor;

  char selName[32], selStats[80], errMsg[128];
  strlcpy(selName, s_selName, sizeof(selName));
  strlcpy(selStats, s_selStats, sizeof(selStats));
  strlcpy(errMsg, s_errMsg, sizeof(errMsg));

  bool needListRebuild = (mode == DockerViewMode::LIST && listSeq != s_renderedListSeq);
  bool needLogUpdate   = (mode == DockerViewMode::LOGS && logSeq != s_renderedLogSeq);

  if (needListRebuild && s_containers && s_snapList && count > 0) {
    memcpy(s_snapList, s_containers, (size_t)count * sizeof(DockerContainer));
  }
  if (needLogUpdate && s_logText && s_snapLogs) {
    strlcpy(s_snapLogs, s_logText, LOG_BUF_SIZE);
  }
  s_dirty = false;
  xSemaphoreGive(s_mux);

  if (s_dotStatus) {
    lv_obj_set_style_bg_color(s_dotStatus, lv_color_hex(stCol), 0);
  }

  if (mode == DockerViewMode::LIST) {
    if (s_btnBack)    lv_obj_add_flag(s_btnBack, LV_OBJ_FLAG_HIDDEN);
    if (s_btnRestart) lv_obj_add_flag(s_btnRestart, LV_OBJ_FLAG_HIDDEN);
    if (s_logPanel)   lv_obj_add_flag(s_logPanel, LV_OBJ_FLAG_HIDDEN);
    if (s_listBox)    lv_obj_clear_flag(s_listBox, LV_OBJ_FLAG_HIDDEN);

    if (s_lblTitle) {
      if (total > 0) lv_label_set_text_fmt(s_lblTitle, "Docker (%d/%d Up)", running, total);
      else           lv_label_set_text(s_lblTitle, "Docker Monitor");
    }

    if (needListRebuild && s_listBox) {
      s_renderedListSeq = listSeq;
      lv_obj_clean(s_listBox);

      if (count == 0) {
        lv_obj_t* lbl = lv_label_create(s_listBox);
        lv_label_set_text(lbl, errMsg[0] ? errMsg : "Mengambil daftar container...");
        lv_obj_set_width(lbl, s_contentW - 4);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(StudioTheme::CHAMPAGNE), 0);
      } else if (s_snapList) {
        for (int i = 0; i < count; i++) {
          uint32_t dotCol = stateColor(s_snapList[i].state, s_snapList[i].status);
          lv_obj_t* btn = lv_btn_create(s_listBox);
          lv_obj_remove_style_all(btn);
          lv_obj_set_width(btn, s_contentW);
          lv_obj_set_height(btn, 38);
          lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
          lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::SURFACE), 0);
          lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::ELEVATED), LV_STATE_PRESSED);
          lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
          lv_obj_set_style_border_color(btn, lv_color_hex(StudioTheme::RULE), 0);
          lv_obj_set_style_border_width(btn, 1, 0);
          lv_obj_set_style_radius(btn, 8, 0);
          lv_obj_set_style_pad_hor(btn, 10, 0);
          lv_obj_set_layout(btn, LV_LAYOUT_FLEX);
          lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
          lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
          lv_obj_set_style_pad_column(btn, 8, 0);
          lv_obj_add_event_cb(btn, onContainerClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);

          lv_obj_t* dot = makePlainBox(btn, 0, 0, 8, 8);
          lv_obj_set_style_bg_color(dot, lv_color_hex(dotCol), 0);
          lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
          lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);

          lv_obj_t* lblName = lv_label_create(btn);
          lv_label_set_text(lblName, s_snapList[i].name);
          lv_obj_set_flex_grow(lblName, 1);
          lv_label_set_long_mode(lblName, LV_LABEL_LONG_DOT);
          lv_obj_set_style_text_font(lblName, &lv_font_montserrat_14, 0);
          lv_obj_set_style_text_color(lblName, lv_color_hex(StudioTheme::TEXT), 0);

          lv_obj_t* lblSt = lv_label_create(btn);
          lv_label_set_text(lblSt, s_snapList[i].status);
          lv_obj_set_width(lblSt, 128);
          lv_label_set_long_mode(lblSt, LV_LABEL_LONG_DOT);
          lv_obj_set_style_text_align(lblSt, LV_TEXT_ALIGN_RIGHT, 0);
          lv_obj_set_style_text_font(lblSt, &lv_font_montserrat_12, 0);
          lv_obj_set_style_text_color(lblSt, lv_color_hex(dotCol), 0);
        }
      }
    }
  } else {
    if (s_btnBack)    lv_obj_clear_flag(s_btnBack, LV_OBJ_FLAG_HIDDEN);
    if (s_btnRestart) lv_obj_clear_flag(s_btnRestart, LV_OBJ_FLAG_HIDDEN);
    if (s_listBox)    lv_obj_add_flag(s_listBox, LV_OBJ_FLAG_HIDDEN);
    if (s_logPanel)   lv_obj_clear_flag(s_logPanel, LV_OBJ_FLAG_HIDDEN);

    if (s_lblTitle) lv_label_set_text(s_lblTitle, selName);
    if (s_lblStats) lv_label_set_text(s_lblStats, selStats);

    if (needLogUpdate && s_lblLogs && s_snapLogs && s_logScroll) {
      s_renderedLogSeq = logSeq;
      size_t logLen = strlen(s_snapLogs);
      bool wasBottom = (lv_obj_get_scroll_bottom(s_logScroll) <= 24);
      lv_label_set_text(s_lblLogs, logLen > 0 ? s_snapLogs : "Memuat log Dozzle...");
      if (wasBottom) {
        lv_obj_update_layout(s_logScroll);
        lv_coord_t sb = lv_obj_get_scroll_bottom(s_logScroll);
        if (sb > 0) {
          lv_obj_scroll_to_y(s_logScroll, lv_obj_get_scroll_y(s_logScroll) + sb, LV_ANIM_OFF);
        }
      }
    }
  }
}

static void onDockerUiTimer(lv_timer_t* timer) {
  (void)timer;
  flushPendingNotification();
  if (s_dirty) renderDockerUi();
}

static void dockerOnBackgroundTick() {
  flushPendingNotification();
}

static lv_obj_t* makeHdrBtn(lv_obj_t* parent, int w, int h, const char* txt, uint32_t col, lv_event_cb_t cb) {
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

static lv_obj_t* dockerOnCreate(lv_obj_t* parent, int rightW, int rightH) {
  ensureDockerBuffers();
  if (!s_mux) {
    s_mux = xSemaphoreCreateMutex();
  }

  s_contentW = rightW - 28;

  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_uiOpen          = true;
    s_renderedListSeq = 0xFFFFFFFFUL;
    s_renderedLogSeq  = 0xFFFFFFFFUL;
    s_reqListSync     = (s_viewMode == DockerViewMode::LIST);
    s_reqLogSync      = (s_viewMode == DockerViewMode::LOGS);
    s_dirty           = true;
    xSemaphoreGive(s_mux);
  }

  s_page = makePlainBox(parent, 0, 0, rightW, rightH);
  lv_obj_set_style_pad_hor(s_page, 14, 0);
  lv_obj_set_style_pad_ver(s_page, 10, 0);
  lv_obj_set_style_pad_row(s_page, 8, 0);
  lv_obj_set_layout(s_page, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);

  // Header
  lv_obj_t* hdr = makePlainBox(s_page, 0, 0, s_contentW, 32);
  lv_obj_set_layout(hdr, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hdr, 6, 0);

  s_btnBack = makeHdrBtn(hdr, 28, 26, LV_SYMBOL_LEFT, StudioTheme::CHAMPAGNE, onBackClick);

  s_dotStatus = makePlainBox(hdr, 0, 0, 8, 8);
  lv_obj_set_style_bg_color(s_dotStatus, lv_color_hex(s_statusColor), 0);
  lv_obj_set_style_bg_opa(s_dotStatus, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_dotStatus, LV_RADIUS_CIRCLE, 0);

  s_lblTitle = lv_label_create(hdr);
  lv_label_set_text(s_lblTitle, "Docker Monitor");
  lv_obj_set_flex_grow(s_lblTitle, 1);
  lv_label_set_long_mode(s_lblTitle, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_lblTitle, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_lblTitle, lv_color_hex(StudioTheme::TEXT), 0);

  lv_obj_t* btnGrp = makePlainBox(hdr, 0, 0, LV_SIZE_CONTENT, 28);
  lv_obj_set_layout(btnGrp, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(btnGrp, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(btnGrp, 6, 0);

  s_btnRestart = makeHdrBtn(btnGrp, 58, 26, "Restart", StudioTheme::CORAL, onRestartClick);
  makeHdrBtn(btnGrp, 28, 26, LV_SYMBOL_REFRESH, StudioTheme::TEAL, onRefreshClick);

  const int bodyH = rightH - 20 - 32 - 8;

  // View 1: Container list
  s_listBox = makePlainBox(s_page, 0, 0, s_contentW, bodyH);
  lv_obj_add_flag(s_listBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s_listBox, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_listBox, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_layout(s_listBox, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_listBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_listBox, 6, 0);

  // View 2: Mini Dozzle Live Logs + Stats
  s_logPanel = makePlainBox(s_page, 0, 0, s_contentW, bodyH);
  lv_obj_set_layout(s_logPanel, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_logPanel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_logPanel, 6, 0);

  s_lblStats = lv_label_create(s_logPanel);
  lv_label_set_text(s_lblStats, s_selStats);
  lv_obj_set_width(s_lblStats, s_contentW);
  lv_label_set_long_mode(s_lblStats, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(s_lblStats, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblStats, lv_color_hex(StudioTheme::TEAL), 0);

  s_logScroll = lv_obj_create(s_logPanel);
  lv_obj_remove_style_all(s_logScroll);
  lv_obj_set_size(s_logScroll, s_contentW, bodyH - 22);
  lv_obj_add_flag(s_logScroll, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s_logScroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_logScroll, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(s_logScroll, lv_color_hex(StudioTheme::TRACK), 0);
  lv_obj_set_style_bg_opa(s_logScroll, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_logScroll, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(s_logScroll, 1, 0);
  lv_obj_set_style_radius(s_logScroll, 10, 0);
  lv_obj_set_style_pad_all(s_logScroll, 8, 0);

  s_lblLogs = lv_label_create(s_logScroll);
  lv_label_set_text(s_lblLogs, "Memuat log...");
  lv_obj_set_width(s_lblLogs, s_contentW - 18);
  lv_label_set_long_mode(s_lblLogs, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(s_lblLogs, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblLogs, lv_color_hex(StudioTheme::TEXT), 0);

  if (!s_taskStarted) {
    BaseType_t ok = xTaskCreatePinnedToCore(dockerPollTask, "docker_mon", 4096, nullptr, 1, nullptr, 0);
    if (ok != pdPASS) {
      ok = xTaskCreatePinnedToCore(dockerPollTask, "docker_mon", 3584, nullptr, 1, nullptr, 0);
    }
    if (ok == pdPASS) {
      s_taskStarted = true;
    } else if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
      char msg[128];
      snprintf(msg, sizeof(msg), "Gagal start task Docker (Heap %uK)", (unsigned)(ESP.getFreeHeap() / 1024));
      setListErrorLocked(msg, StudioTheme::ROSE);
      xSemaphoreGive(s_mux);
    }
  }

  renderDockerUi();
  s_uiTimer = lv_timer_create(onDockerUiTimer, 200, nullptr);
  return s_page;
}

static void dockerOnDestroy() {
  if (s_mux && xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_uiOpen = false;
    xSemaphoreGive(s_mux);
  } else {
    s_uiOpen = false;
  }
  if (s_uiTimer) {
    lv_timer_del(s_uiTimer);
    s_uiTimer = nullptr;
  }
  s_page       = nullptr;
  s_dotStatus  = nullptr;
  s_lblTitle   = nullptr;
  s_btnBack    = nullptr;
  s_btnRestart = nullptr;
  s_listBox    = nullptr;
  s_logPanel   = nullptr;
  s_lblStats   = nullptr;
  s_logScroll  = nullptr;
  s_lblLogs    = nullptr;
}

AppDescriptor appDockerDescriptor() {
  return AppDescriptor{
    "docker",
    "Docker Monitor",
    nullptr,
    &icon_docker,
    false,
    dockerOnCreate,
    dockerOnDestroy,
    nullptr,
    nullptr,
    dockerOnBackgroundTick
  };
}
