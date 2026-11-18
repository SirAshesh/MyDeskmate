#include "ui_manager.h"
#include "boot_screen.h"
#include "notification_shade.h"
#include "page_home.h"
#include "page_antigravity.h"
#include "page_resources.h"
#include "page_timer.h"
#include "page_lyrics.h"
#include "page_settings.h"
#include "icon_antigravity.h"
#include "studio_theme.h"
#include "../hal/display_hx8357d.h"
#include "../hal/touch_4wire.h"
#include "../utils/ui_settings.h"
#include "ui_config.h"

static constexpr int MAX_PAGES = 12;

struct MenuPageEntry {
  lv_obj_t* btn;
  lv_obj_t* icon;
  lv_obj_t* page;
  bool      isImg;
};

static lv_obj_t* s_panelLeft  = nullptr;
static lv_obj_t* s_panelRight = nullptr;
static MenuPageEntry s_pages[MAX_PAGES] = {};
static int s_pageCount = 0;
static int s_activePage = -1;

static void pageAnimYCb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, v);
}

static void onMenuBtnClick(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  uiSetActivePage(idx);
}

static lv_obj_t* makeSidebarBtnBase(lv_obj_t* parent, int /*index*/, int pageIdx) {
  constexpr int BTN_SIZE = 44;
  lv_obj_t* btn = lv_btn_create(parent);
  lv_obj_remove_style_all(btn);
  lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(btn, 1, 0);
  lv_obj_set_style_radius(btn, 12, 0);
  lv_obj_set_style_clip_corner(btn, true, 0);
  lv_obj_add_event_cb(btn, onMenuBtnClick, LV_EVENT_CLICKED, (void*)(intptr_t)pageIdx);
  return btn;
}

static lv_obj_t* makeSidebarBtn(lv_obj_t* parent, int index, const char* symbol, int pageIdx) {
  lv_obj_t* btn = makeSidebarBtnBase(parent, index, pageIdx);
  lv_obj_t* icon = lv_label_create(btn);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(icon, lv_color_hex(StudioTheme::MUTED), 0);
  lv_obj_center(icon);
  return btn;
}

int uiRegisterMenuPage(const char* symbol, lv_obj_t* page) {
  if (!s_panelLeft || !page || s_pageCount >= MAX_PAGES) return -1;
  int idx = s_pageCount++;
  lv_obj_t* btn = makeSidebarBtn(s_panelLeft, idx, symbol, idx);
  s_pages[idx].btn   = btn;
  s_pages[idx].icon  = lv_obj_get_child(btn, 0);
  s_pages[idx].page  = page;
  s_pages[idx].isImg = false;
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  return idx;
}

int uiRegisterMenuPageImg(const lv_img_dsc_t* imgSrc, lv_obj_t* page) {
  if (!s_panelLeft || !imgSrc || !page || s_pageCount >= MAX_PAGES) return -1;
  int idx = s_pageCount++;
  lv_obj_t* btn = makeSidebarBtnBase(s_panelLeft, idx, idx);
  lv_obj_t* icon = lv_img_create(btn);
  lv_img_set_src(icon, imgSrc);
  lv_obj_center(icon);

  s_pages[idx].btn   = btn;
  s_pages[idx].icon  = icon;
  s_pages[idx].page  = page;
  s_pages[idx].isImg = true;
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  return idx;
}

void uiSetActivePage(int pageIdx) {
  bool animateSlide = (s_activePage >= 0 && s_activePage != pageIdx);
  s_activePage = pageIdx;

  for (int i = 0; i < s_pageCount; i++) {
    bool active = (i == pageIdx);
    if (s_pages[i].page) {
      if (active) {
        lv_obj_clear_flag(s_pages[i].page, LV_OBJ_FLAG_HIDDEN);
        if (animateSlide) {
          lv_anim_t a;
          lv_anim_init(&a);
          lv_anim_set_var(&a, s_pages[i].page);
          lv_anim_set_exec_cb(&a, pageAnimYCb);
          lv_anim_set_values(&a, 16, 0);
          lv_anim_set_time(&a, 180);
          lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
          lv_anim_start(&a);
        } else {
          lv_obj_set_y(s_pages[i].page, 0);
        }
      } else {
        lv_obj_add_flag(s_pages[i].page, LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (s_pages[i].btn) {
      lv_obj_set_style_border_color(
        s_pages[i].btn,
        lv_color_hex(active ? StudioTheme::CHAMPAGNE : StudioTheme::RULE),
        0
      );
    }
    if (s_pages[i].icon) {
      if (s_pages[i].isImg) {
        lv_obj_set_style_img_opa(s_pages[i].icon, active ? LV_OPA_COVER : LV_OPA_70, 0);
      } else {
        lv_obj_set_style_text_color(
          s_pages[i].icon,
          lv_color_hex(active ? StudioTheme::CHAMPAGNE : StudioTheme::MUTED),
          0
        );
      }
    }
  }
}

lv_obj_t* uiGetPanelRight() {
  return s_panelRight;
}

void buildUi() {
  uiSettingsLoad();

  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(StudioTheme::BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Sisi kiri: area kecil khusus untuk icon (scrollable vertikal, scrollbar disembunyikan)
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

  // Sisi kanan: area konten utama
  const int rightW = UI_W - UI_LEFT_SIDEBAR_W - 1;
  s_panelRight = makePlainBox(scr, UI_LEFT_SIDEBAR_W + 1, 0, rightW, UI_H);
  lv_obj_set_style_bg_color(s_panelRight, lv_color_hex(StudioTheme::BG), 0);
  lv_obj_set_style_bg_opa(s_panelRight, LV_OPA_COVER, 0);

  // Daftarkan halaman Home, Antigravity Quota, Lyrics, Timer, Resource Manager & Settings
  lv_obj_t* pageHome        = buildHomePage(s_panelRight, rightW, UI_H);
  lv_obj_t* pageAntigravity = buildAntigravityPage(s_panelRight, rightW, UI_H);
  lv_obj_t* pageResources   = buildResourcesPage(s_panelRight, rightW, UI_H);
  lv_obj_t* pageTimer       = buildTimerPage(s_panelRight, rightW, UI_H);
  lv_obj_t* pageLyrics      = buildLyricsPage(s_panelRight, rightW, UI_H);
  lv_obj_t* pageSettings    = buildSettingsPage(s_panelRight, rightW, UI_H);

  uiRegisterMenuPage(LV_SYMBOL_HOME, pageHome);
  uiRegisterMenuPageImg(&icon_antigravity, pageAntigravity);
  uiRegisterMenuPage(LV_SYMBOL_AUDIO, pageLyrics);
  uiRegisterMenuPage(LV_SYMBOL_BELL, pageTimer);
  uiRegisterMenuPage(LV_SYMBOL_DRIVE, pageResources);
  uiRegisterMenuPage(LV_SYMBOL_SETTINGS, pageSettings);

  uiSettingsNotifyAll();
  uiSetActivePage(0);
  notificationShadeInit();
  bootScreenStart();
  initTouchDebugOverlay();
}
