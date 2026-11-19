#include "idle_screen.h"
#include "boot_screen.h"
#include "notification_shade.h"
#include "../app/timer/app_timer.h"
#include "studio_theme.h"
#include "ui_shell.h"
#include "../hal/display_hx8357d.h"
#include "../utils/scaled_font.h"
#include "../utils/ui_settings.h"
#include "ui_config.h"

#include <Arduino.h>
#include <stdio.h>
#include <time.h>

#ifndef UI_IDLE_TIMEOUT_MS
#define UI_IDLE_TIMEOUT_MS (5UL * 60UL * 1000UL)
#endif

namespace {

constexpr int PET_SRC_W  = 16;
constexpr int PET_SRC_H  = 14;
constexpr int PET_SCALE  = 3;
constexpr int PET_W      = PET_SRC_W * PET_SCALE; // 48 px
constexpr int PET_H      = PET_SRC_H * PET_SCALE; // 42 px
constexpr int FLOOR_Y    = 296;
constexpr int PET_Y      = FLOOR_Y - PET_H + 3;   // paws resting on the floor line
constexpr int PET_MIN_X  = 22;
constexpr int PET_MAX_X  = UI_W - 22 - PET_W;

enum class PetMode : uint8_t {
  WALK = 0,
  SIT,
  SLEEP,
};

// 16x14 pixel-art companion cat (facing right; flipped horizontally when moving left)
// Legend:
//   '.' transparent/black   'B' ivory body     'S' warm shadow
//   'E' inner ear champagne 'C' coral collar   'O' teal eye
//   'K' closed eye          'P' rose nose
static const char* const kSpriteWalkA[PET_SRC_H] = {
  "........B....B..",
  ".......BEB..BEB.",
  ".......BBBBBBBB.",
  ".BB...BBBBBBBBBB",
  "B.....BBBBOBBBOB",
  "B.....BBBBBBPBBB",
  ".B....SBBBBBBBB.",
  "..BBBBBBCCCCCC..",
  "..BBBBBBBBBBBB..",
  "..BBBBBBBBBBBB..",
  "..SBBBBBBBBBBS..",
  "..BB........BB..",
  ".BB..........BB.",
  "................",
};

static const char* const kSpriteWalkB[PET_SRC_H] = {
  "........B....B..",
  ".......BEB..BEB.",
  "..B....BBBBBBBB.",
  ".B....BBBBBBBBBB",
  ".B....BBBBOBBBOB",
  ".B....BBBBBBPBBB",
  "..B...SBBBBBBBB.",
  "..BBBBBBCCCCCC..",
  "..BBBBBBBBBBBB..",
  "..BBBBBBBBBBBB..",
  "..SBBBBBBBBBBS..",
  "....BB....BB....",
  "....BB....BB....",
  "................",
};

static const char* const kSpriteWalkC[PET_SRC_H] = {
  "........B....B..",
  ".......BEB..BEB.",
  "...B...BBBBBBBB.",
  "..B...BBBBBBBBBB",
  "..B...BBBBOBBBOB",
  ".B....BBBBBBPBBB",
  "..B...SBBBBBBBB.",
  "..BBBBBBCCCCCC..",
  "..BBBBBBBBBBBB..",
  "..BBBBBBBBBBBB..",
  "..SBBBBBBBBBBS..",
  ".....BB..BB.....",
  "......BB.BB.....",
  "................",
};

static const char* const kSpriteSitOpen[PET_SRC_H] = {
  "................",
  ".......B....B...",
  "......BEB..BEB..",
  "......BBBBBBBB..",
  ".....BBBBBBBBBB.",
  ".....BBBOBBBBOB.",
  ".....BBBBBPBBBB.",
  "......SBBBBBBB..",
  ".....BBCCCCCCB..",
  "....BBBBBBBBBB..",
  "..BBBBBBBBBBBB..",
  ".B..BBBBBBBBBB..",
  "..BBBSBBBBBSBB..",
  "................",
};

static const char* const kSpriteSitBlink[PET_SRC_H] = {
  "................",
  ".......B....B...",
  "......BEB..BEB..",
  "......BBBBBBBB..",
  ".....BBBBBBBBBB.",
  ".....BBBKBBBBKB.",
  ".....BBBBBPBBBB.",
  "......SBBBBBBB..",
  ".....BBCCCCCCB..",
  "....BBBBBBBBBB..",
  "..BBBBBBBBBBBB..",
  ".B..BBBBBBBBBB..",
  "..BBBSBBBBBSBB..",
  "................",
};

static const char* const kSpriteSleepA[PET_SRC_H] = {
  "................",
  "................",
  "................",
  "................",
  "................",
  "........B...B...",
  ".......BEB.BEB..",
  ".....BBBBBBBBBB.",
  "...BBBBBBBBBBBBB",
  "..BBBBBBBKBBBKBB",
  "..SBBBBBBBBPBBBB",
  "..SSBBBBBBBBBBBS",
  "...SSSSSSSSSSSS.",
  "................",
};

static const char* const kSpriteSleepB[PET_SRC_H] = {
  "................",
  "................",
  "................",
  "................",
  "................",
  "........B...B...",
  ".....BBBEB.BEB..",
  "...BBBBBBBBBBBB.",
  "..BBBBBBBBBBBBBB",
  "..BBBBBBBKBBBKBB",
  "..SBBBBBBBBPBBBB",
  "..SSBBBBBBBBBBBS",
  "...SSSSSSSSSSSS.",
  "................",
};

lv_obj_t*   s_root          = nullptr;
lv_obj_t*   s_lblClock      = nullptr;
lv_obj_t*   s_lblDate       = nullptr;
lv_obj_t*   s_petImg        = nullptr;
lv_obj_t*   s_lblZzz        = nullptr;
lv_timer_t* s_guardTimer    = nullptr;
lv_timer_t* s_animTimer     = nullptr;

lv_color_t    s_petBuf[PET_W * PET_H];
lv_img_dsc_t  s_petDsc = {};

bool     s_active           = false;
bool     s_discordActive    = false;
uint32_t s_lastActivityMs   = 0;

// Pet animation state
PetMode  s_petMode          = PetMode::WALK;
int      s_petX             = (UI_W - PET_W) / 2;
int      s_petDir           = 1;     // +1 = right, -1 = left
uint8_t  s_petFrame         = 0;
uint16_t s_petStateTicks    = 0;
uint16_t s_petStateTarget   = 36;
uint32_t s_rngState         = 0x51A7F3C9UL;
int      s_lastSecShown     = -1;

uint32_t nextRand() {
  uint32_t x = s_rngState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s_rngState = x;
  return x;
}

lv_color_t pixelColorFor(char ch) {
  switch (ch) {
    case 'B': return lv_color_hex(0xCFC7BA); // soft warm ivory (dimmed for dark mode)
    case 'S': return lv_color_hex(0x8A8377); // muted shadow
    case 'E': return lv_color_hex(0xC2AA86); // champagne inner ear
    case 'C': return lv_color_hex(0xD97744); // muted coral collar
    case 'O': return lv_color_hex(0x8CC5BF); // teal glowing eye
    case 'K': return lv_color_hex(0x52504D); // closed eye
    case 'P': return lv_color_hex(0xC96868); // soft rose nose
    default:  return lv_color_hex(0x000000); // pitch black background
  }
}

void renderPetSprite(const char* const sprite[PET_SRC_H], bool flipX) {
  for (int sy = 0; sy < PET_SRC_H; sy++) {
    const char* row = sprite[sy];
    for (int sx = 0; sx < PET_SRC_W; sx++) {
      int srcCol = flipX ? (PET_SRC_W - 1 - sx) : sx;
      lv_color_t col = pixelColorFor(row[srcCol]);
      int baseY = sy * PET_SCALE;
      int baseX = sx * PET_SCALE;
      for (int dy = 0; dy < PET_SCALE; dy++) {
        lv_color_t* dst = &s_petBuf[(baseY + dy) * PET_W + baseX];
        for (int dx = 0; dx < PET_SCALE; dx++) {
          dst[dx] = col;
        }
      }
    }
  }
  if (s_petImg) {
    lv_obj_invalidate(s_petImg);
  }
}

bool isIdleBlocked() {
  if (bootScreenIsActive())    return true;
  if (uiIsMusicPageActive())   return true;
  if (uiIsDiscordPageActive()) return true;
  if (s_discordActive)         return true;
  if (timerPageIsRunning())    return true;
  return false;
}

void updateIdleClock(bool force) {
  if (!s_lblClock || !s_lblDate) return;

  time_t now = time(nullptr);
  struct tm tmNow = {};
  localtime_r(&now, &tmNow);

  if (!force && tmNow.tm_sec == s_lastSecShown) return;
  s_lastSecShown = tmNow.tm_sec;

  static const char* const kDays[] = {
    "Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu"
  };
  static const char* const kMonths[] = {
    "Jan", "Feb", "Mar", "Apr", "Mei", "Jun",
    "Jul", "Agu", "Sep", "Okt", "Nov", "Des"
  };

  char buf[32];
  if (uiSettingsGet().showSeconds) {
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
  } else {
    snprintf(buf, sizeof(buf), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
  }
  lv_label_set_text(s_lblClock, buf);

  const char* dayName = (tmNow.tm_wday >= 0 && tmNow.tm_wday < 7) ? kDays[tmNow.tm_wday] : "-";
  const char* monName = (tmNow.tm_mon >= 0 && tmNow.tm_mon < 12) ? kMonths[tmNow.tm_mon] : "-";
  snprintf(buf, sizeof(buf), "%s, %d %s %d", dayName, tmNow.tm_mday, monName, tmNow.tm_year + 1900);
  lv_label_set_text(s_lblDate, buf);
}

void updateZzzBadge() {
  if (!s_lblZzz) return;
  if (s_petMode != PetMode::SLEEP) {
    lv_obj_add_flag(s_lblZzz, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  static const char* const kZzzFrames[] = { "z", "zZ", "zZz", " zZ" };
  uint8_t idx = (s_petFrame / 3) & 3;
  lv_label_set_text(s_lblZzz, kZzzFrames[idx]);
  int zx = s_petX + (s_petDir > 0 ? PET_W - 6 : -14);
  if (zx < 6) zx = 6;
  if (zx > UI_W - 34) zx = UI_W - 34;
  int zy = PET_Y - 6 - (int)(idx * 2);
  lv_obj_set_pos(s_lblZzz, zx, zy);
  lv_obj_clear_flag(s_lblZzz, LV_OBJ_FLAG_HIDDEN);
}

void petAnimTimerCb(lv_timer_t* /*t*/) {
  if (!s_active || !s_root) return;

  updateIdleClock(false);

  s_petFrame++;
  s_petStateTicks++;

  switch (s_petMode) {
    case PetMode::WALK: {
      s_petX += s_petDir * 3;
      if (s_petX <= PET_MIN_X) {
        s_petX   = PET_MIN_X;
        s_petDir = 1;
      } else if (s_petX >= PET_MAX_X) {
        s_petX   = PET_MAX_X;
        s_petDir = -1;
      }

      static const char* const* kWalkCycle[4] = {
        kSpriteWalkA, kSpriteWalkB, kSpriteWalkC, kSpriteWalkB
      };
      renderPetSprite(kWalkCycle[s_petFrame & 3], s_petDir < 0);
      if (s_petImg) {
        int bobY = (s_petFrame & 1) ? (PET_Y - 1) : PET_Y;
        lv_obj_set_pos(s_petImg, s_petX, bobY);
      }

      if (s_petStateTicks >= s_petStateTarget) {
        s_petStateTicks = 0;
        uint32_t r = nextRand() % 10;
        if (r < 5) {
          s_petMode        = PetMode::SIT;
          s_petStateTarget = 18 + (uint16_t)(nextRand() % 20);
        } else if (r < 7) {
          s_petMode        = PetMode::SLEEP;
          s_petStateTarget = 32 + (uint16_t)(nextRand() % 28);
        } else {
          // Turn around and keep walking
          s_petDir         = -s_petDir;
          s_petStateTarget = 28 + (uint16_t)(nextRand() % 40);
        }
      }
      break;
    }

    case PetMode::SIT: {
      bool blink = ((s_petFrame % 18) == 0 || (s_petFrame % 18) == 1);
      renderPetSprite(blink ? kSpriteSitBlink : kSpriteSitOpen, s_petDir < 0);
      if (s_petImg) {
        lv_obj_set_pos(s_petImg, s_petX, PET_Y);
      }
      if (s_petStateTicks >= s_petStateTarget) {
        s_petStateTicks = 0;
        if ((nextRand() % 4) == 0) {
          s_petMode        = PetMode::SLEEP;
          s_petStateTarget = 30 + (uint16_t)(nextRand() % 26);
        } else {
          if ((nextRand() % 2) == 0) s_petDir = -s_petDir;
          s_petMode        = PetMode::WALK;
          s_petStateTarget = 32 + (uint16_t)(nextRand() % 48);
        }
      }
      break;
    }

    case PetMode::SLEEP: {
      bool breathe = ((s_petFrame / 5) & 1) != 0;
      renderPetSprite(breathe ? kSpriteSleepB : kSpriteSleepA, s_petDir < 0);
      if (s_petImg) {
        lv_obj_set_pos(s_petImg, s_petX, PET_Y);
      }
      if (s_petStateTicks >= s_petStateTarget) {
        s_petStateTicks  = 0;
        s_petMode        = PetMode::WALK;
        s_petDir         = ((nextRand() & 1) == 0) ? 1 : -1;
        s_petStateTarget = 34 + (uint16_t)(nextRand() % 46);
      }
      break;
    }
  }

  updateZzzBadge();
}

void idleGuardTimerCb(lv_timer_t* /*t*/) {
  uint32_t now = millis();

  if (isIdleBlocked()) {
    s_lastActivityMs = now;
    if (s_active) {
      idleScreenExit();
    }
    return;
  }

  if (!s_active && (now - s_lastActivityMs >= UI_IDLE_TIMEOUT_MS)) {
    idleScreenEnter();
  }
}

void animOpaCb(void* var, int32_t v) {
  lv_obj_set_style_opa((lv_obj_t*)var, (lv_opa_t)v, 0);
}

void onFadeOutReady(lv_anim_t* /*a*/) {
  if (!s_active && s_root) {
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  }
}

} // namespace

void idleScreenInit() {
  if (s_root) return;

  s_lastActivityMs = millis();
  s_rngState ^= (millis() + 0x9E3779B9UL);

  s_petDsc.header.always_zero = 0;
  s_petDsc.header.w           = PET_W;
  s_petDsc.header.h           = PET_H;
  s_petDsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
  s_petDsc.data_size          = sizeof(s_petBuf);
  s_petDsc.data               = reinterpret_cast<const uint8_t*>(s_petBuf);

  lv_obj_t* top = lv_layer_top();
  s_root = makePlainBox(top, 0, 0, UI_W, UI_H);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

  // Clock container centered in upper-middle area
  lv_obj_t* clockBox = makePlainBox(s_root, 0, 44, UI_W, 175);
  lv_obj_set_layout(clockBox, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(clockBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(clockBox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(clockBox, 10, 0);

  s_lblClock = lv_label_create(clockBox);
  lv_obj_set_style_text_font(s_lblClock, getScaledClockFont(), 0);
  lv_obj_set_style_text_color(s_lblClock, lv_color_hex(0xC8C4BC), 0);
  lv_obj_set_style_text_letter_space(s_lblClock, 2, 0);
  lv_label_set_text(s_lblClock, "00:00");

  s_lblDate = lv_label_create(clockBox);
  lv_obj_set_style_text_font(s_lblDate, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(s_lblDate, lv_color_hex(0x6E6A63), 0);
  lv_label_set_text(s_lblDate, "");

  // Walking pixel-art pet
  renderPetSprite(kSpriteWalkA, false);
  s_petImg = lv_img_create(s_root);
  lv_img_set_src(s_petImg, &s_petDsc);
  lv_obj_set_pos(s_petImg, s_petX, PET_Y);

  // Subtle dark horizon line right underneath the pet's paws
  lv_obj_t* floorLine = makePlainBox(s_root, 18, FLOOR_Y, UI_W - 36, 1);
  lv_obj_set_style_bg_color(floorLine, lv_color_hex(0x1B1B1F), 0);
  lv_obj_set_style_bg_opa(floorLine, LV_OPA_COVER, 0);

  // Tiny floating zZz badge when pet naps
  s_lblZzz = lv_label_create(s_root);
  lv_obj_set_style_text_font(s_lblZzz, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblZzz, lv_color_hex(StudioTheme::CHAMPAGNE), 0);
  lv_label_set_text(s_lblZzz, "zZz");
  lv_obj_add_flag(s_lblZzz, LV_OBJ_FLAG_HIDDEN);

  s_guardTimer = lv_timer_create(idleGuardTimerCb, 500, nullptr);
  s_animTimer  = lv_timer_create(petAnimTimerCb, 110, nullptr);
  lv_timer_pause(s_animTimer);
}

void idleScreenNotifyActivity() {
  s_lastActivityMs = millis();
}

bool idleScreenIsActive() {
  return s_active;
}

void idleScreenSetDiscordActive(bool active) {
  s_discordActive = active;
  if (active) {
    s_lastActivityMs = millis();
    if (s_active) {
      idleScreenExit();
    }
  }
}

void idleScreenEnter() {
  if (!s_root || s_active) return;

  if (notificationShadeIsOpen()) {
    notificationShadeClose();
  }

  s_active        = true;
  s_lastSecShown  = -1;
  s_petMode       = PetMode::WALK;
  s_petStateTicks = 0;
  s_petStateTarget = 34 + (uint16_t)(nextRand() % 36);
  s_petFrame      = 0;

  // Refresh font in case content scale changed in Settings
  if (s_lblClock) {
    lv_obj_set_style_text_font(s_lblClock, getScaledClockFont(), 0);
  }
  updateIdleClock(true);
  renderPetSprite(kSpriteWalkA, s_petDir < 0);
  if (s_petImg) {
    lv_obj_set_pos(s_petImg, s_petX, PET_Y);
  }
  updateZzzBadge();

  lv_anim_del(s_root, animOpaCb);
  lv_obj_move_foreground(s_root);
  lv_obj_clear_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(s_root, LV_OPA_TRANSP, 0);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_root);
  lv_anim_set_exec_cb(&a, animOpaCb);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_time(&a, 320);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  if (s_animTimer) {
    lv_timer_resume(s_animTimer);
  }
  Serial.println("[IDLE] Entered idle mode");
}

void idleScreenExit() {
  s_lastActivityMs = millis();
  if (!s_root || !s_active) return;

  s_active = false;
  if (s_animTimer) {
    lv_timer_pause(s_animTimer);
  }

  lv_anim_del(s_root, animOpaCb);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_root);
  lv_anim_set_exec_cb(&a, animOpaCb);
  lv_anim_set_values(&a, lv_obj_get_style_opa(s_root, 0), LV_OPA_TRANSP);
  lv_anim_set_time(&a, 180);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, onFadeOutReady);
  lv_anim_start(&a);

  Serial.println("[IDLE] Exited idle mode");
}
