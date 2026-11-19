#include "pc_mouse.h"
#include "studio_theme.h"
#include "../hal/display_hx8357d.h"
#include "ui_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <lvgl.h>

namespace {
constexpr uint16_t PC_MOUSE_PORT = 8789;
constexpr uint32_t PC_MOUSE_TIMEOUT_MS = 5000;

WiFiUDP   s_udp;
bool      s_udpStarted = false;
bool      s_active     = false;
bool      s_leftDown   = false;
int16_t   s_x          = UI_W / 2;
int16_t   s_y          = UI_H / 2;
uint32_t  s_lastPktMs  = 0;

lv_obj_t* s_cursorObj  = nullptr;
} // namespace

static lv_obj_t* findScrollableAt(int16_t x, int16_t y) {
  lv_point_t pt = { x, y };
  lv_obj_t* obj = lv_indev_search_obj(lv_scr_act(), &pt);
  while (obj) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) &&
        (lv_obj_get_scroll_dir(obj) & LV_DIR_VER)) {
      return obj;
    }
    obj = lv_obj_get_parent(obj);
  }
  return nullptr;
}

void pcMouseInit() {
  if (s_cursorObj) return;
  lv_obj_t* top = lv_layer_top();

  // Kursor panah / pointer kontras tinggi di atas semua aplikasi
  s_cursorObj = lv_obj_create(top);
  lv_obj_remove_style_all(s_cursorObj);
  lv_obj_set_size(s_cursorObj, 12, 12);
  lv_obj_set_style_radius(s_cursorObj, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_cursorObj, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(s_cursorObj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_cursorObj, lv_color_hex(StudioTheme::BG), 0);
  lv_obj_set_style_border_width(s_cursorObj, 2, 0);
  lv_obj_clear_flag(s_cursorObj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_cursorObj, LV_OBJ_FLAG_HIDDEN);
}

void pcMouseLoop() {
  if (WiFi.status() == WL_CONNECTED && !s_udpStarted) {
    s_udp.begin(PC_MOUSE_PORT);
    s_udpStarted = true;
    Serial.printf("[PC-MOUSE] Listening UDP %s:%u\n",
                  WiFi.localIP().toString().c_str(), PC_MOUSE_PORT);
  }

  if (!s_udpStarted) return;

  bool dirty = false;
  int pktSize = 0;
  while ((pktSize = s_udp.parsePacket()) > 0) {
    uint8_t buf[16];
    int n = s_udp.read(buf, sizeof(buf));
    if (n >= 9 && buf[0] == 'P' && buf[1] == 'M') {
      bool nextActive = (buf[2] != 0);
      bool nextLeft   = (buf[3] & 0x01) != 0;
      int16_t nx = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
      int16_t ny = (int16_t)((uint16_t)buf[6] | ((uint16_t)buf[7] << 8));
      int8_t wheel = (int8_t)buf[8];

      nx = constrain(nx, 0, UI_W - 1);
      ny = constrain(ny, 0, UI_H - 1);

      if (s_active != nextActive || s_leftDown != (nextActive && nextLeft) || s_x != nx || s_y != ny) {
        dirty = true;
      }

      s_active    = nextActive;
      s_leftDown  = nextActive && nextLeft;
      s_x         = nx;
      s_y         = ny;
      s_lastPktMs = millis();

      if (s_active && wheel != 0) {
        lv_obj_t* target = findScrollableAt(s_x, s_y);
        if (target) {
          lv_obj_scroll_by_bounded(target, 0, (lv_coord_t)wheel * 42, LV_ANIM_ON);
        }
      }
    }
  }

  if (dirty && s_cursorObj) {
    if (s_active) {
      lv_obj_set_pos(s_cursorObj, s_x - 6, s_y - 6);
      lv_obj_set_style_bg_color(
        s_cursorObj,
        s_leftDown ? lv_color_hex(StudioTheme::TEAL) : lv_color_white(),
        0
      );
      lv_obj_clear_flag(s_cursorObj, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_cursorObj, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (s_active && (millis() - s_lastPktMs > PC_MOUSE_TIMEOUT_MS)) {
    s_active   = false;
    s_leftDown = false;
    if (s_cursorObj) {
      lv_obj_add_flag(s_cursorObj, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

bool pcMouseGetState(int16_t& outX, int16_t& outY, bool& outLeftDown) {
  if (!s_active) return false;
  outX        = s_x;
  outY        = s_y;
  outLeftDown = s_leftDown;
  return true;
}
