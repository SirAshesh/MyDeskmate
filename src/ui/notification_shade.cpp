#include "notification_shade.h"
#include "boot_screen.h"
#include "../hal/display_hx8357d.h"
#include "../utils/mqtt_notify.h"
#include "ui_config.h"
#include <stdio.h>
#include <string.h>

namespace {
constexpr int MAX_NOTIFICATIONS = 12;
constexpr int SWIPE_START_TOP_MAX_Y = 68;
constexpr int SWIPE_OPEN_THRESHOLD_Y = 44;
constexpr int SWIPE_CLOSE_THRESHOLD_Y = 44;
constexpr int SHADE_ANIM_MS = 260;

struct NotificationItem {
  char     title[48];
  char     body[128];
  char     timeStr[16];
  char     key[96];
  uint32_t accentHex;
};

NotificationItem s_items[MAX_NOTIFICATIONS];
int s_count = 0;

lv_obj_t* s_topHintPill = nullptr;
lv_obj_t* s_shade       = nullptr;
lv_obj_t* s_countBadge  = nullptr;
lv_obj_t* s_clearBtn    = nullptr;
lv_obj_t* s_list        = nullptr;
lv_obj_t* s_emptyBox    = nullptr;

bool s_isOpen    = false;
bool s_animating = false;

// Gesture tracking via pointer indev
bool    s_tracking    = false;
bool    s_dragging    = false;
int16_t s_startX      = 0;
int16_t s_startY      = 0;
int16_t s_lastDy      = 0;
} // namespace

static void animYCb(void* var, int32_t v) {
  lv_obj_set_y((lv_obj_t*)var, v);
}

static void onOpenAnimReady(lv_anim_t* /*a*/) {
  s_animating = false;
}

static void onCloseAnimReady(lv_anim_t* /*a*/) {
  s_animating = false;
  if (s_shade) {
    lv_obj_add_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
  }
  if (s_topHintPill) {
    lv_obj_clear_flag(s_topHintPill, LV_OBJ_FLAG_HIDDEN);
  }
}

static void rebuildNotificationList() {
  if (!s_list || !s_emptyBox) return;

  // Hapus semua kartu lama (sisakan s_emptyBox di index 0)
  uint32_t childCnt = lv_obj_get_child_cnt(s_list);
  for (int i = (int)childCnt - 1; i >= 0; i--) {
    lv_obj_t* child = lv_obj_get_child(s_list, i);
    if (child != s_emptyBox) {
      lv_obj_del(child);
    }
  }

  if (s_countBadge) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_count);
    lv_label_set_text(s_countBadge, buf);
  }

  if (s_clearBtn) {
    if (s_count > 0) lv_obj_clear_flag(s_clearBtn, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_add_flag(s_clearBtn, LV_OBJ_FLAG_HIDDEN);
  }

  if (s_topHintPill) {
    lv_obj_set_style_bg_color(
      s_topHintPill,
      lv_color_hex(s_count > 0 ? StudioTheme::CHAMPAGNE : StudioTheme::SUBTLE),
      0
    );
  }

  if (s_count == 0) {
    lv_obj_clear_flag(s_emptyBox, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  lv_obj_add_flag(s_emptyBox, LV_OBJ_FLAG_HIDDEN);

  // Tampilkan notifikasi terbaru di paling atas
  for (int i = s_count - 1; i >= 0; i--) {
    const NotificationItem& item = s_items[i];

    lv_obj_t* card = lv_obj_create(s_list);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(StudioTheme::ELEVATED), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(StudioTheme::RULE), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_left(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Garis aksen vertikal di sisi kiri kartu
    lv_obj_t* accentBar = makePlainBox(card, -6, 2, 3, 28);
    lv_obj_set_style_bg_color(accentBar, lv_color_hex(item.accentHex), 0);
    lv_obj_set_style_bg_opa(accentBar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(accentBar, 2, 0);

    // Baris atas: judul + waktu
    lv_obj_t* topRow = makePlainBox(card, 0, 0, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(topRow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(topRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* lblTitle = lv_label_create(topRow);
    lv_label_set_text(lblTitle, item.title);
    lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lblTitle, lv_color_hex(StudioTheme::TEXT), 0);
    lv_obj_set_flex_grow(lblTitle, 1);
    lv_label_set_long_mode(lblTitle, LV_LABEL_LONG_DOT);

    if (item.timeStr[0] != '\0') {
      lv_obj_t* lblTime = lv_label_create(topRow);
      lv_label_set_text(lblTime, item.timeStr);
      lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(lblTime, lv_color_hex(StudioTheme::SUBTLE), 0);
      lv_obj_set_style_pad_right(lblTime, 6, 0);
    }

    // Tombol hapus per kartu (sync ke HP)
    lv_obj_t* btnDismiss = lv_btn_create(topRow);
    lv_obj_remove_style_all(btnDismiss);
    lv_obj_set_size(btnDismiss, 22, 22);
    lv_obj_set_style_bg_color(btnDismiss, lv_color_hex(StudioTheme::SURFACE), 0);
    lv_obj_set_style_bg_color(btnDismiss, lv_color_hex(StudioTheme::RULE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btnDismiss, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btnDismiss, 6, 0);
    lv_obj_add_event_cb(btnDismiss, onDismissCardClick, LV_EVENT_CLICKED, (void*)(intptr_t)i);

    lv_obj_t* dismissLbl = lv_label_create(btnDismiss);
    lv_label_set_text(dismissLbl, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(dismissLbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(dismissLbl, lv_color_hex(StudioTheme::MUTED), 0);
    lv_obj_center(dismissLbl);

    if (item.body[0] != '\0') {
      lv_obj_t* lblBody = lv_label_create(card);
      lv_label_set_text(lblBody, item.body);
      lv_obj_set_pos(lblBody, 0, 22);
      lv_obj_set_width(lblBody, LV_PCT(100));
      lv_label_set_long_mode(lblBody, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_font(lblBody, &lv_font_montserrat_12, 0);
      lv_obj_set_style_text_color(lblBody, lv_color_hex(StudioTheme::MUTED), 0);
    }
  }
}

static void rebuildNotificationList();

static void onDismissCardClick(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= s_count) return;
  if (s_items[idx].key[0] != '\0') {
    mqttNotifySendDismiss(s_items[idx].key);
  }
  for (int i = idx + 1; i < s_count; i++) {
    s_items[i - 1] = s_items[i];
  }
  s_count--;
  rebuildNotificationList();
}

static void onClearAllClick(lv_event_t* /*e*/) {
  notificationClearAll(true);
}

static void onCloseClick(lv_event_t* /*e*/) {
  notificationShadeClose();
}

static void gesturePollTimerCb(lv_timer_t* /*t*/) {
  if (bootScreenIsActive() || !s_shade) return;

  lv_indev_t* indev = lv_indev_get_next(nullptr);
  if (!indev) return;

  lv_point_t pt;
  lv_indev_get_point(indev, &pt);
  bool pressed = (indev->proc.state == LV_INDEV_STATE_PRESSED);

  if (pressed) {
    if (!s_tracking) {
      s_tracking = true;
      s_dragging = false;
      s_startX   = pt.x;
      s_startY   = pt.y;
      s_lastDy   = 0;
    } else if (!s_animating) {
      int16_t dx    = pt.x - s_startX;
      int16_t dy    = pt.y - s_startY;
      int16_t absDx = dx < 0 ? -dx : dx;
      int16_t absDy = dy < 0 ? -dy : dy;
      s_lastDy      = dy;

      if (!s_isOpen) {
        if (!s_dragging && s_startY <= SWIPE_START_TOP_MAX_Y && dy >= 12 && absDy > absDx) {
          s_dragging = true;
          lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(s_shade);
          if (s_topHintPill) lv_obj_add_flag(s_topHintPill, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_dragging) {
          int newY = -UI_H + dy;
          if (newY > 0) newY = 0;
          if (newY < -UI_H) newY = -UI_H;
          lv_obj_set_y(s_shade, newY);
        }
      } else {
        if (!s_dragging && dy <= -12 && absDy > absDx) {
          bool allowClose = (s_startY >= UI_H - 64) || !s_list || (lv_obj_get_scroll_bottom(s_list) <= 2);
          if (allowClose) {
            s_dragging = true;
          }
        }
        if (s_dragging) {
          int newY = dy;
          if (newY > 0) newY = 0;
          if (newY < -UI_H) newY = -UI_H;
          lv_obj_set_y(s_shade, newY);
        }
      }
    }
  } else {
    if (s_tracking && s_dragging && !s_animating) {
      if (!s_isOpen) {
        if (s_lastDy >= SWIPE_OPEN_THRESHOLD_Y) {
          notificationShadeOpen();
        } else {
          // Snap kembali tertutup dengan animasi halus
          s_isOpen = true;
          notificationShadeClose();
        }
      } else {
        if (s_lastDy <= -SWIPE_CLOSE_THRESHOLD_Y) {
          notificationShadeClose();
        } else {
          // Snap kembali terbuka dengan animasi halus
          s_isOpen = false;
          notificationShadeOpen();
        }
      }
    }
    s_tracking = false;
    s_dragging = false;
    s_lastDy   = 0;
  }
}

void notificationShadeInit() {
  lv_obj_t* topLayer = lv_layer_top();

  // Indikator pill tipis di bagian atas layar sebagai petunjuk swipe-down
  s_topHintPill = makePlainBox(topLayer, (UI_W - 44) / 2, 4, 44, 4);
  lv_obj_set_style_bg_color(s_topHintPill, lv_color_hex(StudioTheme::SUBTLE), 0);
  lv_obj_set_style_bg_opa(s_topHintPill, LV_OPA_70, 0);
  lv_obj_set_style_radius(s_topHintPill, 2, 0);

  // Container utama notification shade (full screen di layer teratas)
  s_shade = makePlainBox(topLayer, 0, -UI_H, UI_W, UI_H);
  lv_obj_set_style_bg_color(s_shade, lv_color_hex(StudioTheme::BG), 0);
  lv_obj_set_style_bg_opa(s_shade, LV_OPA_90, 0);
  lv_obj_add_flag(s_shade, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);

  // Header shade
  constexpr int HEADER_H = 44;
  lv_obj_t* header = makePlainBox(s_shade, 0, 0, UI_W, HEADER_H);
  lv_obj_set_style_bg_color(header, lv_color_hex(StudioTheme::HEADER), 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(header, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(header, 1, 0);
  lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);

  lv_obj_t* hdrIcon = lv_label_create(header);
  lv_label_set_text(hdrIcon, LV_SYMBOL_BELL);
  lv_obj_set_pos(hdrIcon, 16, 13);
  lv_obj_set_style_text_font(hdrIcon, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(hdrIcon, lv_color_hex(StudioTheme::CHAMPAGNE), 0);

  lv_obj_t* hdrTitle = lv_label_create(header);
  lv_label_set_text(hdrTitle, "Notifikasi");
  lv_obj_set_pos(hdrTitle, 40, 12);
  lv_obj_set_style_text_font(hdrTitle, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(hdrTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_countBadge = lv_label_create(header);
  lv_label_set_text(s_countBadge, "0");
  lv_obj_set_pos(s_countBadge, 130, 14);
  lv_obj_set_style_text_font(s_countBadge, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_countBadge, lv_color_hex(StudioTheme::MUTED), 0);

  // Tombol "Hapus Semua"
  s_clearBtn = lv_btn_create(header);
  lv_obj_remove_style_all(s_clearBtn);
  lv_obj_set_pos(s_clearBtn, UI_W - 154, 8);
  lv_obj_set_size(s_clearBtn, 96, 28);
  lv_obj_set_style_bg_color(s_clearBtn, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(s_clearBtn, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(s_clearBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s_clearBtn, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(s_clearBtn, 1, 0);
  lv_obj_set_style_radius(s_clearBtn, 8, 0);
  lv_obj_add_event_cb(s_clearBtn, onClearAllClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* clearLbl = lv_label_create(s_clearBtn);
  lv_label_set_text(clearLbl, "Hapus");
  lv_obj_set_style_text_font(clearLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(clearLbl, lv_color_hex(StudioTheme::TEXT), 0);
  lv_obj_center(clearLbl);

  // Tombol tutup ("X")
  lv_obj_t* closeBtn = lv_btn_create(header);
  lv_obj_remove_style_all(closeBtn);
  lv_obj_set_pos(closeBtn, UI_W - 48, 8);
  lv_obj_set_size(closeBtn, 36, 28);
  lv_obj_set_style_bg_color(closeBtn, lv_color_hex(StudioTheme::ELEVATED), 0);
  lv_obj_set_style_bg_color(closeBtn, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(closeBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(closeBtn, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(closeBtn, 1, 0);
  lv_obj_set_style_radius(closeBtn, 8, 0);
  lv_obj_add_event_cb(closeBtn, onCloseClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* closeLbl = lv_label_create(closeBtn);
  lv_label_set_text(closeLbl, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_font(closeLbl, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(closeLbl, lv_color_hex(StudioTheme::MUTED), 0);
  lv_obj_center(closeLbl);

  // Area list notifikasi (scrollable)
  constexpr int BOTTOM_BAR_H = 26;
  const int listH = UI_H - HEADER_H - BOTTOM_BAR_H;
  s_list = lv_obj_create(s_shade);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_pos(s_list, 0, HEADER_H);
  lv_obj_set_size(s_list, UI_W, listH);
  lv_obj_set_layout(s_list, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(s_list, 12, 0);
  lv_obj_set_style_pad_row(s_list, 8, 0);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

  // Empty state: "Tidak ada notifikasi"
  s_emptyBox = makePlainBox(s_list, 0, 0, UI_W - 24, listH - 24);
  lv_obj_set_layout(s_emptyBox, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_emptyBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_emptyBox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_emptyBox, 10, 0);

  lv_obj_t* emptyIcon = lv_label_create(s_emptyBox);
  lv_label_set_text(emptyIcon, LV_SYMBOL_BELL);
  lv_obj_set_style_text_font(emptyIcon, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(emptyIcon, lv_color_hex(StudioTheme::SUBTLE), 0);

  lv_obj_t* emptyText = lv_label_create(s_emptyBox);
  lv_label_set_text(emptyText, "Tidak ada notifikasi");
  lv_obj_set_style_text_font(emptyText, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(emptyText, lv_color_hex(StudioTheme::MUTED), 0);

  // Handle bar di bagian bawah shade (bisa di-tap atau swipe ke atas untuk tutup)
  lv_obj_t* bottomBar = lv_btn_create(s_shade);
  lv_obj_remove_style_all(bottomBar);
  lv_obj_set_pos(bottomBar, 0, UI_H - BOTTOM_BAR_H);
  lv_obj_set_size(bottomBar, UI_W, BOTTOM_BAR_H);
  lv_obj_set_style_bg_color(bottomBar, lv_color_hex(StudioTheme::FOOTER), 0);
  lv_obj_set_style_bg_opa(bottomBar, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(bottomBar, onCloseClick, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* handlePill = makePlainBox(bottomBar, (UI_W - 56) / 2, (BOTTOM_BAR_H - 4) / 2, 56, 4);
  lv_obj_set_style_bg_color(handlePill, lv_color_hex(StudioTheme::MUTED), 0);
  lv_obj_set_style_bg_opa(handlePill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(handlePill, 2, 0);

  rebuildNotificationList();

  lv_timer_create(gesturePollTimerCb, 20, nullptr);
}

void notificationShadeOpen() {
  if (!s_shade || s_isOpen) return;
  s_isOpen    = true;
  s_animating = true;

  if (s_topHintPill) {
    lv_obj_add_flag(s_topHintPill, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_clear_flag(s_shade, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_shade);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_shade);
  lv_anim_set_exec_cb(&a, animYCb);
  lv_anim_set_values(&a, lv_obj_get_y(s_shade), 0);
  lv_anim_set_time(&a, SHADE_ANIM_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, onOpenAnimReady);
  lv_anim_start(&a);
}

void notificationShadeClose() {
  if (!s_shade || !s_isOpen) return;
  s_isOpen    = false;
  s_animating = true;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_shade);
  lv_anim_set_exec_cb(&a, animYCb);
  lv_anim_set_values(&a, lv_obj_get_y(s_shade), -UI_H);
  lv_anim_set_time(&a, SHADE_ANIM_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, onCloseAnimReady);
  lv_anim_start(&a);
}

bool notificationShadeIsOpen() {
  return s_isOpen;
}

void notificationPush(const char* title,
                      const char* body,
                      const char* timeStr,
                      uint32_t accentHex,
                      const char* syncKey) {
  // Jika key sudah ada, update item tersebut agar tidak duplikat
  if (syncKey && syncKey[0] != '\0') {
    for (int i = 0; i < s_count; i++) {
      if (strcmp(s_items[i].key, syncKey) == 0) {
        snprintf(s_items[i].title, sizeof(s_items[i].title), "%s", title ? title : "Notifikasi");
        snprintf(s_items[i].body, sizeof(s_items[i].body), "%s", body ? body : "");
        snprintf(s_items[i].timeStr, sizeof(s_items[i].timeStr), "%s", timeStr ? timeStr : "");
        s_items[i].accentHex = accentHex;
        rebuildNotificationList();
        return;
      }
    }
  }

  if (s_count >= MAX_NOTIFICATIONS) {
    // Geser membuang item paling lama
    for (int i = 1; i < MAX_NOTIFICATIONS; i++) {
      s_items[i - 1] = s_items[i];
    }
    s_count = MAX_NOTIFICATIONS - 1;
  }

  NotificationItem& item = s_items[s_count++];
  snprintf(item.title, sizeof(item.title), "%s", title ? title : "Notifikasi");
  snprintf(item.body, sizeof(item.body), "%s", body ? body : "");
  snprintf(item.timeStr, sizeof(item.timeStr), "%s", timeStr ? timeStr : "");
  snprintf(item.key, sizeof(item.key), "%s", syncKey ? syncKey : "");
  item.accentHex = accentHex;

  rebuildNotificationList();
}

bool notificationRemoveByKey(const char* syncKey) {
  if (!syncKey || syncKey[0] == '\0') return false;
  for (int i = 0; i < s_count; i++) {
    if (strcmp(s_items[i].key, syncKey) == 0) {
      for (int j = i + 1; j < s_count; j++) {
        s_items[j - 1] = s_items[j];
      }
      s_count--;
      rebuildNotificationList();
      return true;
    }
  }
  return false;
}

void notificationClearAll(bool syncToPhone) {
  if (syncToPhone && s_count > 0) {
    mqttNotifySendClearAll();
  }
  s_count = 0;
  rebuildNotificationList();
}

int notificationCount() {
  return s_count;
}
