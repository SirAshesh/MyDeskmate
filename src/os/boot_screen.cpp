#include "boot_screen.h"
#include "studio_theme.h"
#include "../hal/display_hx8357d.h"
#include "wifi_config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include <string.h>
#include <time.h>

namespace {
enum BootState {
  BOOT_INIT = 0,
  BOOT_WIFI,
  BOOT_NTP_WITA,
  BOOT_DONE,
};

constexpr uint32_t WIFI_TIMEOUT_MS = 10000;
constexpr uint32_t NTP_TIMEOUT_MS  = 8000;

lv_obj_t*   s_root      = nullptr;
lv_obj_t*   s_bar       = nullptr;
lv_obj_t*   s_lblStatus = nullptr;
lv_timer_t* s_timer     = nullptr;

BootState s_state       = BOOT_INIT;
uint32_t  s_stateStart  = 0;
bool      s_active      = false;
bool      s_wifiStarted = false;
bool      s_ntpStarted  = false;
} // namespace

static bool hasValidWifiCreds() {
  return (WIFI_SSID[0] != '\0') && (strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0);
}

static void setBootProgress(int32_t pct, const char* statusText) {
  if (s_bar) {
    lv_bar_set_value(s_bar, pct, LV_ANIM_ON);
  }
  if (s_lblStatus && statusText) {
    lv_label_set_text(s_lblStatus, statusText);
  }
}

static void animSlideOutYCb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, v);
}

static void onSlideOutReady(lv_anim_t* /*a*/) {
  s_active = false;
  if (s_root) {
    lv_obj_del_async(s_root);
    s_root      = nullptr;
    s_bar       = nullptr;
    s_lblStatus = nullptr;
  }
}

static void finishBootScreen() {
  if (s_timer) {
    lv_timer_del(s_timer);
    s_timer = nullptr;
  }
  if (!s_root) {
    s_active = false;
    return;
  }

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_root);
  lv_anim_set_exec_cb(&a, animSlideOutYCb);
  lv_anim_set_values(&a, 0, -UI_H);
  lv_anim_set_time(&a, 280);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, onSlideOutReady);
  lv_anim_start(&a);
}

static void bootTimerCb(lv_timer_t* /*t*/) {
  uint32_t now = millis();
  uint32_t elapsed = now - s_stateStart;

  switch (s_state) {
    case BOOT_INIT: {
      if (elapsed >= 350) {
        s_state      = BOOT_WIFI;
        s_stateStart = now;
        setBootProgress(25, "Menghubungkan ke WiFi...");
      }
      break;
    }

    case BOOT_WIFI: {
      if (!hasValidWifiCreds()) {
        setBootProgress(60, "WiFi belum diatur");
        s_state      = BOOT_DONE;
        s_stateStart = now;
        break;
      }

      if (!s_wifiStarted) {
        s_wifiStarted = true;
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }

      if (WiFi.status() == WL_CONNECTED) {
        setBootProgress(60, "WiFi terhubung");
        s_state      = BOOT_NTP_WITA;
        s_stateStart = now;
      } else if (elapsed >= WIFI_TIMEOUT_MS) {
        setBootProgress(60, "WiFi gagal, mode offline");
        s_state      = BOOT_DONE;
        s_stateStart = now;
      } else {
        int pct = 25 + (int)((elapsed * 30UL) / WIFI_TIMEOUT_MS);
        if (pct > 55) pct = 55;
        lv_bar_set_value(s_bar, pct, LV_ANIM_ON);
      }
      break;
    }

    case BOOT_NTP_WITA: {
      if (!s_ntpStarted) {
        s_ntpStarted = true;
        setBootProgress(68, "Mengatur waktu asli WITA...");
        // WITA = UTC+8 (8 * 3600 detik)
        configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov", "id.pool.ntp.org");
        setenv("TZ", "WITA-8", 1);
        tzset();
      }

      time_t tNow = time(nullptr);
      if (tNow >= 1700000000L) {
        setBootProgress(100, "Waktu WITA tersinkron");
        s_state      = BOOT_DONE;
        s_stateStart = now;
      } else if (elapsed >= NTP_TIMEOUT_MS) {
        setBootProgress(100, "Menggunakan waktu lokal");
        s_state      = BOOT_DONE;
        s_stateStart = now;
      } else {
        int pct = 68 + (int)((elapsed * 28UL) / NTP_TIMEOUT_MS);
        if (pct > 96) pct = 96;
        lv_bar_set_value(s_bar, pct, LV_ANIM_ON);
      }
      break;
    }

    case BOOT_DONE: {
      if (elapsed >= 450) {
        finishBootScreen();
      }
      break;
    }
  }
}

void bootScreenStart() {
  if (s_active) return;
  s_active      = true;
  s_state       = BOOT_INIT;
  s_stateStart  = millis();
  s_wifiStarted = false;
  s_ntpStarted  = false;

  lv_obj_t* topLayer = lv_layer_top();
  s_root = makePlainBox(topLayer, 0, 0, UI_W, UI_H);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(StudioTheme::BG), 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_layout(s_root, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_root, 16, 0);

  lv_obj_t* lblBrand = lv_label_create(s_root);
  lv_label_set_text(lblBrand, "MyDeskmate");
  lv_obj_set_style_text_font(lblBrand, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(lblBrand, lv_color_hex(StudioTheme::CHAMPAGNE), 0);

  s_bar = lv_bar_create(s_root);
  lv_obj_remove_style_all(s_bar);
  lv_obj_set_size(s_bar, 260, 8);
  lv_obj_set_style_bg_color(s_bar, lv_color_hex(StudioTheme::ELEVATED), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_bar, lv_color_hex(StudioTheme::RULE), LV_PART_MAIN);
  lv_obj_set_style_border_width(s_bar, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(s_bar, 4, LV_PART_MAIN);

  lv_obj_set_style_bg_color(s_bar, lv_color_hex(StudioTheme::CHAMPAGNE), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(s_bar, 4, LV_PART_INDICATOR);
  lv_obj_set_style_anim_time(s_bar, 300, 0);
  lv_bar_set_range(s_bar, 0, 100);
  lv_bar_set_value(s_bar, 10, LV_ANIM_ON);

  s_lblStatus = lv_label_create(s_root);
  lv_label_set_text(s_lblStatus, "Menyiapkan sistem...");
  lv_obj_set_style_text_font(s_lblStatus, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_lblStatus, lv_color_hex(StudioTheme::MUTED), 0);

  s_timer = lv_timer_create(bootTimerCb, 50, nullptr);
}

bool bootScreenIsActive() {
  return s_active;
}
