#include "ui_shell.h"
#include "app_manager.h"
#include "boot_screen.h"
#include "notification_shade.h"
#include "pc_mouse.h"
#include "studio_theme.h"

#include "../app/home/app_home.h"
#include "../app/antigravity/app_antigravity.h"
#include "../app/discord/app_discord.h"
#include "../app/music/app_music.h"
#include "../app/timer/app_timer.h"
#include "../app/monitor/app_monitor.h"
#include "../app/system_manager/app_system_manager.h"
#include "../app/settings/app_settings.h"

#include "../hal/display_hx8357d.h"
#include "../hal/touch_4wire.h"
#include "../utils/ui_settings.h"
#include "ui_config.h"

static lv_obj_t* s_panelLeft  = nullptr;
static lv_obj_t* s_panelRight = nullptr;

lv_obj_t* uiShellGetPanelRight() {
  return s_panelRight;
}

void uiShellInit() {
  uiSettingsLoad();

  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(StudioTheme::BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Sisi kiri: sidebar icon menu (scrollable vertikal, scrollbar disembunyikan)
  s_panelLeft = makePlainBox(scr, 0, 0, UI_LEFT_SIDEBAR_W, UI_H);
  lv_obj_add_flag(s_panelLeft, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(s_panelLeft, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_panelLeft, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_layout(s_panelLeft, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_panelLeft, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_panelLeft, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_ver(s_panelLeft, 8, 0);
  lv_obj_set_style_pad_row(s_panelLeft, 7, 0);
  lv_obj_set_style_bg_color(s_panelLeft, lv_color_hex(StudioTheme::HEADER), 0);
  lv_obj_set_style_bg_opa(s_panelLeft, LV_OPA_COVER, 0);

  // Garis pemisah tipis antara sisi kiri & sisi kanan
  lv_obj_t* divider = makePlainBox(scr, UI_LEFT_SIDEBAR_W, 0, 1, UI_H);
  lv_obj_set_style_bg_color(divider, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

  // Sisi kanan: area konten aplikasi
  const int rightW = UI_W - UI_LEFT_SIDEBAR_W - 1;
  s_panelRight = makePlainBox(scr, UI_LEFT_SIDEBAR_W + 1, 0, rightW, UI_H);
  lv_obj_set_style_bg_color(s_panelRight, lv_color_hex(StudioTheme::BG), 0);
  lv_obj_set_style_bg_opa(s_panelRight, LV_OPA_COVER, 0);

  // Inisialisasi App Manager & Crash Guard (B)
  appManagerInit(s_panelLeft, s_panelRight, rightW, UI_H);

  // Daftarkan aplikasi secara terisolasi (Lazy-loaded kecuali Home)
  appManagerRegister(appHomeDescriptor());
  appManagerRegister(appAntigravityDescriptor());
  appManagerRegister(appDiscordDescriptor());
  appManagerRegister(appMusicDescriptor());
  appManagerRegister(appTimerDescriptor());
  appManagerRegister(appMonitorDescriptor());
  appManagerRegister(appSystemManagerDescriptor());
  appManagerRegister(appSettingsDescriptor());

  notificationShadeInit();
  appManagerOpen(0);
  uiSettingsNotifyAll();
  bootScreenStart();
  pcMouseInit();
  initTouchDebugOverlay();
}

void uiShellLoop() {
  pcMouseLoop();
  appManagerLoop();
}
