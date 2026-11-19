#include "app_manager.h"
#include "notification_shade.h"
#include "studio_theme.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <string.h>

struct AppRuntimeEntry {
  AppDescriptor desc;
  lv_obj_t*     btn;
  lv_obj_t*     icon;
  lv_obj_t*     page;
  bool          isImg;
  bool          quarantined;
};

static lv_obj_t*       s_panelLeft  = nullptr;
static lv_obj_t*       s_panelRight = nullptr;
static int             s_rightW     = 0;
static int             s_rightH     = 0;
static AppRuntimeEntry s_apps[OS_MAX_APPS] = {};
static int             s_appCount   = 0;
static int             s_activeIdx  = -1;

static lv_obj_t*       s_quarantinePage = nullptr;
static uint32_t        s_quarantineMask = 0;
static char            s_lastCrashName[48] = "";
static char            s_bootNotifyMsg[112] = "";
static bool            s_pendingCrashNotify = false;

static void pageAnimYCb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, v);
}

static void saveActiveIdNvs(const char* appId) {
  Preferences prefs;
  if (prefs.begin("os_guard", false)) {
    prefs.putString("active_id", appId ? appId : "");
    prefs.end();
  }
}

static void saveQuarantineMaskNvs() {
  Preferences prefs;
  if (prefs.begin("os_guard", false)) {
    prefs.putUInt("q_mask", s_quarantineMask);
    prefs.putString("last_crash", s_lastCrashName);
    prefs.end();
  }
}

static bool isAbnormalCrashReset(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC ||
         r == ESP_RST_INT_WDT ||
         r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT;
}

const char* appManagerGetLastResetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "Power On";
    case ESP_RST_EXT:       return "External Reset";
    case ESP_RST_SW:        return "Software Reset";
    case ESP_RST_PANIC:     return "Panic / Exception";
    case ESP_RST_INT_WDT:   return "Interrupt WDT";
    case ESP_RST_TASK_WDT:  return "Task WDT";
    case ESP_RST_WDT:       return "Other WDT";
    case ESP_RST_DEEPSLEEP: return "Deep Sleep";
    case ESP_RST_BROWNOUT:  return "Brownout";
    case ESP_RST_SDIO:      return "SDIO Reset";
    default:                return "Unknown";
  }
}

static void updateSidebarStyles() {
  for (int i = 0; i < s_appCount; i++) {
    bool active = (i == s_activeIdx);
    bool quar   = s_apps[i].quarantined;

    if (s_apps[i].btn) {
      uint32_t borderCol = StudioTheme::RULE;
      if (quar) {
        borderCol = StudioTheme::ROSE;
      } else if (active) {
        borderCol = StudioTheme::CHAMPAGNE;
      }
      lv_obj_set_style_border_color(s_apps[i].btn, lv_color_hex(borderCol), 0);
    }

    if (s_apps[i].icon) {
      if (s_apps[i].isImg) {
        lv_obj_set_style_img_opa(s_apps[i].icon,
                                 quar ? LV_OPA_30 : (active ? LV_OPA_COVER : LV_OPA_70), 0);
      } else {
        uint32_t textCol = StudioTheme::MUTED;
        if (quar) {
          textCol = StudioTheme::ROSE;
        } else if (active) {
          textCol = StudioTheme::CHAMPAGNE;
        }
        lv_obj_set_style_text_color(s_apps[i].icon, lv_color_hex(textCol), 0);
      }
    }
  }
}

static void onSidebarBtnClick(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  appManagerOpen(idx);
}

static lv_obj_t* makeSidebarBtnBase(int idx) {
  constexpr int BTN_SIZE = 44;
  lv_obj_t* btn = lv_btn_create(s_panelLeft);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_clip_corner(btn, true, 0);
  lv_obj_add_event_cb(btn, onSidebarBtnClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
  return btn;
}

static void destroyQuarantinePage() {
  if (s_quarantinePage) {
    lv_obj_del(s_quarantinePage);
    s_quarantinePage = nullptr;
  }
}

static void destroyAppUi(int idx) {
  if (idx < 0 || idx >= s_appCount) return;
  if (s_apps[idx].desc.onDestroy) {
    s_apps[idx].desc.onDestroy();
  }
  if (s_apps[idx].page) {
    lv_obj_del(s_apps[idx].page);
    s_apps[idx].page = nullptr;
  }
}

static void onRestoreQuarantineClick(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  appManagerUnquarantine(idx);
  appManagerOpen(idx);
}

static void showQuarantinePage(int idx) {
  destroyQuarantinePage();

  s_quarantinePage = makePlainBox(s_panelRight, 0, 0, s_rightW, s_rightH);
  lv_obj_set_layout(s_quarantinePage, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_quarantinePage, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_quarantinePage, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(s_quarantinePage, 24, 0);
  lv_obj_set_style_pad_row(s_quarantinePage, 12, 0);

  lv_obj_t* card = lv_obj_create(s_quarantinePage);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, s_rightW - 60, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(card, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(StudioTheme::ROSE), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_pad_all(card, 18, 0);
  lv_obj_set_layout(card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(card, 10, 0);

  lv_obj_t* lblIcon = lv_label_create(card);
  lv_label_set_text(lblIcon, LV_SYMBOL_WARNING);
  lv_obj_set_style_text_font(lblIcon, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(lblIcon, lv_color_hex(StudioTheme::ROSE), 0);

  lv_obj_t* lblTitle = lv_label_create(card);
  lv_label_set_text_fmt(lblTitle, "%s Dikarantina", s_apps[idx].desc.name);
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lblTitle, lv_color_hex(StudioTheme::TEXT), 0);
  lv_obj_set_style_text_align(lblTitle, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t* lblDesc = lv_label_create(card);
  lv_label_set_text(lblDesc,
                    "Crash Guard menghentikan aplikasi ini karena memicu restart tidak normal pada sesi sebelumnya. Menu lain tetap aman.");
  lv_obj_set_width(lblDesc, s_rightW - 104);
  lv_label_set_long_mode(lblDesc, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(lblDesc, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblDesc, lv_color_hex(StudioTheme::MUTED), 0);
  lv_obj_set_style_text_align(lblDesc, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t* btnRestore = lv_btn_create(card);
  lv_obj_remove_style_all(btnRestore);
  lv_obj_set_size(btnRestore, 180, 38);
  lv_obj_set_style_bg_color(btnRestore, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(btnRestore, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btnRestore, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btnRestore, lv_color_hex(StudioTheme::TEAL), 0);
  lv_obj_set_style_border_width(btnRestore, 1, 0);
  lv_obj_set_style_radius(btnRestore, 10, 0);
  lv_obj_add_event_cb(btnRestore, onRestoreQuarantineClick, LV_EVENT_CLICKED, (void*)(intptr_t)idx);

  lv_obj_t* lblBtn = lv_label_create(btnRestore);
  lv_label_set_text(lblBtn, "Pulihkan & Buka");
  lv_obj_set_style_text_font(lblBtn, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblBtn, lv_color_hex(StudioTheme::TEAL), 0);
  lv_obj_center(lblBtn);
}

void appManagerInit(lv_obj_t* panelLeft, lv_obj_t* panelRight, int rightW, int rightH) {
  s_panelLeft  = panelLeft;
  s_panelRight = panelRight;
  s_rightW     = rightW;
  s_rightH     = rightH;
  s_appCount   = 0;
  s_activeIdx  = -1;
  s_quarantinePage = nullptr;
  s_pendingCrashNotify = false;

  Preferences prefs;
  String prevActiveId = "";
  if (prefs.begin("os_guard", true)) {
    s_quarantineMask = prefs.getUInt("q_mask", 0);
    prevActiveId     = prefs.getString("active_id", "");
    String lastCrash = prefs.getString("last_crash", "");
    strlcpy(s_lastCrashName, lastCrash.c_str(), sizeof(s_lastCrashName));
    prefs.end();
  }

  esp_reset_reason_t reason = esp_reset_reason();
  if (isAbnormalCrashReset(reason) && prevActiveId.length() > 0 && prevActiveId != "home") {
    // Akan dicocokkan saat appManagerRegister dipanggil
    strlcpy(s_lastCrashName, prevActiveId.c_str(), sizeof(s_lastCrashName));
    s_pendingCrashNotify = true;
  } else {
    s_pendingCrashNotify = false;
  }

  // Bersihkan active_id agar reboot normal tidak menandai app
  saveActiveIdNvs("");
}

int appManagerRegister(const AppDescriptor& desc) {
  if (!s_panelLeft || !s_panelRight || s_appCount >= OS_MAX_APPS) return -1;
  int idx = s_appCount++;

  s_apps[idx].desc        = desc;
  s_apps[idx].page        = nullptr;
  s_apps[idx].isImg       = (desc.iconImg != nullptr);
  s_apps[idx].quarantined = (s_quarantineMask & (1UL << idx)) != 0;

  // Cek apakah app ini yang menyebabkan crash sebelum reboot
  if (s_pendingCrashNotify && desc.id && strcmp(desc.id, s_lastCrashName) == 0 &&
      strcmp(desc.id, "home") != 0) {
    s_quarantineMask |= (1UL << idx);
    s_apps[idx].quarantined = true;
    strlcpy(s_lastCrashName, desc.name ? desc.name : desc.id, sizeof(s_lastCrashName));
    saveQuarantineMaskNvs();
    snprintf(s_bootNotifyMsg, sizeof(s_bootNotifyMsg),
             "App '%s' dikarantina (%s)", s_lastCrashName, appManagerGetLastResetReasonStr());
    Serial.printf("[CrashGuard] %s\n", s_bootNotifyMsg);
  }

  lv_obj_t* btn = makeSidebarBtnBase(idx);
  lv_obj_t* icon = nullptr;
  if (s_apps[idx].isImg) {
    icon = lv_img_create(btn);
    lv_img_set_src(icon, desc.iconImg);
    lv_obj_center(icon);
  } else {
    icon = lv_label_create(btn);
    lv_label_set_text(icon, desc.symbol ? desc.symbol : LV_SYMBOL_FILE);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(StudioTheme::MUTED), 0);
    lv_obj_center(icon);
  }

  s_apps[idx].btn  = btn;
  s_apps[idx].icon = icon;
  updateSidebarStyles();
  return idx;
}

void appManagerOpen(int idx) {
  if (idx < 0 || idx >= s_appCount) return;

  if (s_pendingCrashNotify && s_bootNotifyMsg[0] != '\0') {
    notificationPush("Crash Guard", s_bootNotifyMsg, nullptr, StudioTheme::ROSE);
    s_pendingCrashNotify = false;
  }

  if (idx == s_activeIdx && (s_apps[idx].page != nullptr || s_apps[idx].quarantined)) {
    return;
  }

  int prevIdx = s_activeIdx;
  bool animateSlide = (prevIdx >= 0 && prevIdx != idx);

  destroyQuarantinePage();

  // Pause & bersihkan app sebelumnya jika bukan keepAlive
  if (prevIdx >= 0 && prevIdx < s_appCount) {
    if (s_apps[prevIdx].desc.onPause) {
      s_apps[prevIdx].desc.onPause();
    }
    if (!s_apps[prevIdx].desc.keepAlive) {
      destroyAppUi(prevIdx);
    } else if (s_apps[prevIdx].page) {
      lv_obj_add_flag(s_apps[prevIdx].page, LV_OBJ_FLAG_HIDDEN);
    }
  }

  s_activeIdx = idx;

  if (s_apps[idx].quarantined) {
    saveActiveIdNvs("");
    showQuarantinePage(idx);
    updateSidebarStyles();
    return;
  }

  // Catat active_id di NVS sebelum menjalankan onCreate app non-home
  if (s_apps[idx].desc.id && strcmp(s_apps[idx].desc.id, "home") != 0) {
    saveActiveIdNvs(s_apps[idx].desc.id);
  } else {
    saveActiveIdNvs("");
  }

  // Lazy Load: buat UI hanya saat dibuka
  if (!s_apps[idx].page && s_apps[idx].desc.onCreate) {
    s_apps[idx].page = s_apps[idx].desc.onCreate(s_panelRight, s_rightW, s_rightH);
  }

  if (s_apps[idx].page) {
    lv_obj_clear_flag(s_apps[idx].page, LV_OBJ_FLAG_HIDDEN);
    if (animateSlide) {
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, s_apps[idx].page);
      lv_anim_set_exec_cb(&a, pageAnimYCb);
      lv_anim_set_values(&a, 16, 0);
      lv_anim_set_time(&a, 180);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
      lv_anim_start(&a);
    } else {
      lv_obj_set_y(s_apps[idx].page, 0);
    }
  }

  if (s_apps[idx].desc.onResume) {
    s_apps[idx].desc.onResume();
  }

  updateSidebarStyles();
}

void appManagerLoop() {
  for (int i = 0; i < s_appCount; i++) {
    if (!s_apps[i].quarantined && s_apps[i].desc.onBackgroundTick) {
      s_apps[i].desc.onBackgroundTick();
    }
  }
}

int appManagerGetCount() {
  return s_appCount;
}

int appManagerGetActiveIndex() {
  return s_activeIdx;
}

const AppDescriptor* appManagerGetDescriptor(int idx) {
  if (idx < 0 || idx >= s_appCount) return nullptr;
  return &s_apps[idx].desc;
}

bool appManagerIsQuarantined(int idx) {
  if (idx < 0 || idx >= s_appCount) return false;
  return s_apps[idx].quarantined;
}

int appManagerGetQuarantineCount() {
  int count = 0;
  for (int i = 0; i < s_appCount; i++) {
    if (s_apps[i].quarantined) count++;
  }
  return count;
}

const char* appManagerGetLastCrashName() {
  return s_lastCrashName[0] ? s_lastCrashName : "-";
}

void appManagerUnquarantine(int idx) {
  if (idx < 0 || idx >= s_appCount) return;
  s_apps[idx].quarantined = false;
  s_quarantineMask &= ~(1UL << idx);
  saveQuarantineMaskNvs();
  updateSidebarStyles();
}

void appManagerClearAllQuarantine() {
  s_quarantineMask = 0;
  s_lastCrashName[0] = '\0';
  for (int i = 0; i < s_appCount; i++) {
    s_apps[i].quarantined = false;
  }
  saveQuarantineMaskNvs();
  updateSidebarStyles();
}
