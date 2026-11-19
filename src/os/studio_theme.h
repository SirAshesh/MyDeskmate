#pragma once

#include <lvgl.h>
#include <stdint.h>

// ============================================================
// PALET TEMA CLAWPAL STUDIO (CLEAN DARK MODERN)
// ============================================================
namespace StudioTheme {
  constexpr uint32_t BG        = 0x0E0E10;
  constexpr uint32_t HEADER    = 0x121215;
  constexpr uint32_t FOOTER    = 0x0A0A0B;
  constexpr uint32_t SURFACE   = 0x151517;
  constexpr uint32_t ELEVATED  = 0x1F1F22;
  constexpr uint32_t TRACK     = 0x0B0B0D;
  constexpr uint32_t RULE      = 0x2A2A2E;

  constexpr uint32_t TEXT      = 0xECEBE8;
  constexpr uint32_t MUTED     = 0x8D8C89;
  constexpr uint32_t SUBTLE    = 0x5E5D5A;

  constexpr uint32_t CHAMPAGNE = 0xD8C3A0;
  constexpr uint32_t TEAL      = 0x8CC5BF;
  constexpr uint32_t CORAL     = 0xFF8B4F;
  constexpr uint32_t SAGE      = 0xA8BDA6;
  constexpr uint32_t ROSE      = 0xE57373;
}

inline lv_obj_t* makePlainBox(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* obj = lv_obj_create(parent);
  lv_obj_remove_style_all(obj);
  lv_obj_set_pos(obj, x, y);
  lv_obj_set_size(obj, w, h);
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return obj;
}
