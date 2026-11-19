#include "app_monitor.h"
#include "../../os/studio_theme.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr int      MON_W    = 411;
constexpr int      MON_H    = 320;
constexpr size_t   MON_SIZE = MON_W * MON_H * 2;
constexpr uint16_t MON_PORT = 8788;

lv_obj_t*   s_page      = nullptr;
lv_obj_t*   s_img       = nullptr;
lv_obj_t*   s_waitBox   = nullptr;
lv_obj_t*   s_lblIp     = nullptr;
lv_timer_t* s_uiTimer   = nullptr;

uint8_t*      s_fb         = nullptr;
uint8_t       s_tileBuf[32 * 32 * 2] = {};
lv_img_dsc_t  s_imgDsc     = {};
bool          s_taskStarted = false;
volatile bool s_connected  = false;
volatile bool s_frameDirty = false;
volatile bool s_uiVisible  = false;
volatile int16_t s_dirtyX1 = MON_W;
volatile int16_t s_dirtyY1 = MON_H;
volatile int16_t s_dirtyX2 = -1;
volatile int16_t s_dirtyY2 = -1;
} // namespace

static void ensureMonitorFb() {
  if (s_fb) return;
  s_fb = (uint8_t*)heap_caps_calloc(1, MON_SIZE, MALLOC_CAP_SPIRAM);
  if (!s_fb) s_fb = (uint8_t*)calloc(1, MON_SIZE);

  s_imgDsc.header.always_zero = 0;
  s_imgDsc.header.reserved    = 0;
  s_imgDsc.header.w           = MON_W;
  s_imgDsc.header.h           = MON_H;
  s_imgDsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
  s_imgDsc.data_size          = MON_SIZE;
  s_imgDsc.data               = s_fb;
}

static bool readExact(WiFiClient& client, uint8_t* dst, size_t len, uint32_t timeoutMs = 1500) {
  size_t got = 0;
  uint32_t start = millis();
  while (got < len) {
    if (!client.connected()) return false;
    int avail = client.available();
    if (avail > 0) {
      int n = client.read(dst + got, (size_t)avail < (len - got) ? (size_t)avail : (len - got));
      if (n > 0) {
        got += (size_t)n;
        start = millis();
      }
    } else {
      if (millis() - start > timeoutMs) return false;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
  return true;
}

static bool skipExact(WiFiClient& client, size_t len) {
  uint8_t scratch[256];
  size_t left = len;
  while (left > 0) {
    size_t chunk = left > sizeof(scratch) ? sizeof(scratch) : left;
    if (!readExact(client, scratch, chunk)) return false;
    left -= chunk;
  }
  return true;
}

static void monitorNetTask(void* param) {
  (void)param;
  WiFiServer server(MON_PORT);
  bool serverStarted = false;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED && !serverStarted) {
      server.begin();
      server.setNoDelay(true);
      serverStarted = true;
      Serial.printf("[MON] Listening on %s:%u\n", WiFi.localIP().toString().c_str(), MON_PORT);
    }

    if (serverStarted) {
      WiFiClient client = server.available();
      if (client) {
        client.setNoDelay(true);
        s_connected = true;
        Serial.println("[MON] PC connected");

        while (client.connected()) {
          uint8_t hdr[6];
          if (!readExact(client, hdr, 6, 5000)) {
            break;
          }
          if (hdr[0] != 'M' || hdr[1] != 'F' || hdr[2] != 'R' || hdr[3] != 'M') {
            break;
          }
          uint16_t tileCount = (uint16_t)hdr[4] | ((uint16_t)hdr[5] << 8);
          bool frameOk = true;
          int16_t minX = MON_W, minY = MON_H, maxX = -1, maxY = -1;
          for (uint16_t t = 0; t < tileCount; t++) {
            uint8_t th[8];
            if (!readExact(client, th, 8, 5000)) {
              frameOk = false;
              break;
            }
            uint16_t x = (uint16_t)th[0] | ((uint16_t)th[1] << 8);
            uint16_t y = (uint16_t)th[2] | ((uint16_t)th[3] << 8);
            uint16_t w = (uint16_t)th[4] | ((uint16_t)th[5] << 8);
            uint16_t h = (uint16_t)th[6] | ((uint16_t)th[7] << 8);
            size_t bytes = (size_t)w * (size_t)h * 2U;

            if (!s_fb || x + w > MON_W || y + h > MON_H || w == 0 || h == 0 || bytes > sizeof(s_tileBuf)) {
              if (!skipExact(client, bytes)) {
                frameOk = false;
                break;
              }
              continue;
            }

            if (!readExact(client, s_tileBuf, bytes, 5000)) {
              frameOk = false;
              break;
            }
            const size_t rowBytes = (size_t)w * 2U;
            for (uint16_t row = 0; row < h; row++) {
              uint8_t* rowPtr = s_fb + (((size_t)(y + row) * MON_W + x) * 2U);
              memcpy(rowPtr, s_tileBuf + ((size_t)row * rowBytes), rowBytes);
            }
            if ((int16_t)x < minX) minX = (int16_t)x;
            if ((int16_t)y < minY) minY = (int16_t)y;
            if ((int16_t)(x + w - 1) > maxX) maxX = (int16_t)(x + w - 1);
            if ((int16_t)(y + h - 1) > maxY) maxY = (int16_t)(y + h - 1);
          }
          if (!frameOk) break;
          if (tileCount > 0 && s_uiVisible && maxX >= minX && maxY >= minY) {
            if (minX < s_dirtyX1) s_dirtyX1 = minX;
            if (minY < s_dirtyY1) s_dirtyY1 = minY;
            if (maxX > s_dirtyX2) s_dirtyX2 = maxX;
            if (maxY > s_dirtyY2) s_dirtyY2 = maxY;
            s_frameDirty = true;
          }
          uint8_t ack = 0x06;
          client.write(&ack, 1);
        }
        client.stop();
        s_connected = false;
        Serial.println("[MON] PC disconnected");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

static void onMonitorUiTimer(lv_timer_t* timer) {
  (void)timer;
  if (!s_page || !s_img) return;

  if (s_waitBox) {
    if (s_connected) {
      lv_obj_add_flag(s_waitBox, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(s_waitBox, LV_OBJ_FLAG_HIDDEN);
      if (s_lblIp) {
        if (WiFi.status() == WL_CONNECTED) {
          lv_label_set_text_fmt(s_lblIp, "python server/second_monitor.py --ip %s",
                                WiFi.localIP().toString().c_str());
        } else {
          lv_label_set_text(s_lblIp, "WiFi belum terhubung");
        }
      }
    }
  }

  if (s_frameDirty && s_fb) {
    s_frameDirty = false;
    int16_t x1 = s_dirtyX1, y1 = s_dirtyY1, x2 = s_dirtyX2, y2 = s_dirtyY2;
    s_dirtyX1 = MON_W; s_dirtyY1 = MON_H;
    s_dirtyX2 = -1;    s_dirtyY2 = -1;
    if (x2 >= x1 && y2 >= y1) {
      lv_area_t area;
      area.x1 = s_img->coords.x1 + x1;
      area.y1 = s_img->coords.y1 + y1;
      area.x2 = s_img->coords.x1 + x2;
      area.y2 = s_img->coords.y1 + y2;
      lv_obj_invalidate_area(s_img, &area);
    } else {
      lv_obj_invalidate(s_img);
    }
  }
}

static lv_obj_t* monitorOnCreate(lv_obj_t* parent, int rightW, int rightH) {
  ensureMonitorFb();
  s_uiVisible = true;

  s_page = makePlainBox(parent, 0, 0, rightW, rightH);

  s_img = lv_img_create(s_page);
  if (s_fb) {
    lv_img_set_src(s_img, &s_imgDsc);
  }
  lv_obj_set_pos(s_img, 0, 0);

  s_waitBox = makePlainBox(s_page, 0, 0, rightW, rightH);
  lv_obj_set_style_bg_color(s_waitBox, lv_color_hex(StudioTheme::BG), 0);
  lv_obj_set_style_bg_opa(s_waitBox, LV_OPA_90, 0);
  lv_obj_set_layout(s_waitBox, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_waitBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_waitBox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(s_waitBox, 20, 0);
  lv_obj_set_style_pad_row(s_waitBox, 10, 0);

  lv_obj_t* lblTitle = lv_label_create(s_waitBox);
  lv_label_set_text(lblTitle, "Second Monitor (WiFi)");
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblTitle, lv_color_hex(StudioTheme::TEXT), 0);

  lv_obj_t* lblSub = lv_label_create(s_waitBox);
  lv_label_set_text(lblSub, "Stream layar kedua + kursor Windows ke ESP32:");
  lv_obj_set_style_text_font(lblSub, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblSub, lv_color_hex(StudioTheme::MUTED), 0);

  s_lblIp = lv_label_create(s_waitBox);
  lv_obj_set_width(s_lblIp, rightW - 40);
  lv_label_set_long_mode(s_lblIp, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(s_lblIp, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(s_lblIp, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lblIp, lv_color_hex(StudioTheme::TEAL), 0);

  if (!s_taskStarted) {
    s_taskStarted = true;
    xTaskCreatePinnedToCore(monitorNetTask, "mon_net", 6144, nullptr, 2, nullptr, 0);
  }

  s_uiTimer = lv_timer_create(onMonitorUiTimer, 16, nullptr);
  onMonitorUiTimer(nullptr);
  return s_page;
}

static void monitorOnDestroy() {
  s_uiVisible = false;
  if (s_uiTimer) {
    lv_timer_del(s_uiTimer);
    s_uiTimer = nullptr;
  }
  s_page    = nullptr;
  s_img     = nullptr;
  s_waitBox = nullptr;
  s_lblIp   = nullptr;
}

AppDescriptor appMonitorDescriptor() {
  return AppDescriptor{
    "monitor",
    "Second Monitor",
    LV_SYMBOL_IMAGE,
    nullptr,
    false,
    monitorOnCreate,
    monitorOnDestroy,
    nullptr,
    nullptr,
    nullptr
  };
}
