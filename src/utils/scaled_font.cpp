#include "scaled_font.h"
#include "ui_settings.h"
#include <Arduino.h>
#include <math.h>

static lv_font_t s_clockFontBig;
static uint8_t   s_scaledGlyphBuf[12288];
static constexpr int CLOCK_SCALE_DEN = 400;

static inline int clockScaleNum() {
  return 7 * uiSettingsGet().scalePct;
}

static uint8_t sampleSrcGlyph4bpp(const uint8_t* bmp, uint16_t w, uint16_t h, int x, int y, uint8_t bpp) {
  if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) return 0;
  uint32_t idx = (uint32_t)y * w + (uint32_t)x;
  if (bpp == 8) return bmp[idx];
  if (bpp == 4) {
    uint8_t byteVal = bmp[idx >> 1];
    uint8_t nib = (idx & 1) ? (byteVal & 0x0F) : ((byteVal >> 4) & 0x0F);
    return (uint8_t)(nib * 17);
  }
  return 0;
}

static bool scaledClockGetDsc(const lv_font_t* font_p, lv_font_glyph_dsc_t* dsc_out,
                              uint32_t letter, uint32_t letter_next) {
  const lv_font_t* src = &lv_font_montserrat_48;
  lv_font_glyph_dsc_t sd = {};
  if (!src->get_glyph_dsc(src, &sd, letter, letter_next)) return false;

  const int num = clockScaleNum();
  dsc_out->resolved_font  = font_p;
  dsc_out->adv_w          = (uint16_t)((sd.adv_w * num + CLOCK_SCALE_DEN / 2) / CLOCK_SCALE_DEN);
  dsc_out->box_w          = (uint16_t)((sd.box_w * num + CLOCK_SCALE_DEN / 2) / CLOCK_SCALE_DEN);
  dsc_out->box_h          = (uint16_t)((sd.box_h * num + CLOCK_SCALE_DEN / 2) / CLOCK_SCALE_DEN);
  dsc_out->ofs_x          = (int16_t)((sd.ofs_x * num) / CLOCK_SCALE_DEN);
  dsc_out->ofs_y          = (int16_t)((sd.ofs_y * num) / CLOCK_SCALE_DEN);
  dsc_out->bpp            = 8;
  dsc_out->is_placeholder = sd.is_placeholder;
  return true;
}

static const uint8_t* scaledClockGetBitmap(const lv_font_t* font_p, uint32_t letter) {
  (void)font_p;
  const lv_font_t* src = &lv_font_montserrat_48;
  lv_font_glyph_dsc_t sd = {};
  if (!src->get_glyph_dsc(src, &sd, letter, 0)) return nullptr;
  const uint8_t* sb = src->get_glyph_bitmap(src, letter);
  if (!sb || sd.box_w == 0 || sd.box_h == 0) return nullptr;

  const int num = clockScaleNum();
  uint16_t ow = (uint16_t)((sd.box_w * num + CLOCK_SCALE_DEN / 2) / CLOCK_SCALE_DEN);
  uint16_t oh = (uint16_t)((sd.box_h * num + CLOCK_SCALE_DEN / 2) / CLOCK_SCALE_DEN);
  if (ow == 0 || oh == 0 || (uint32_t)ow * oh > sizeof(s_scaledGlyphBuf)) return nullptr;

  for (uint16_t oy = 0; oy < oh; oy++) {
    float sy = ((oy + 0.5f) * sd.box_h) / (float)oh - 0.5f;
    int y0 = (int)floorf(sy);
    float fy = sy - y0;
    for (uint16_t ox = 0; ox < ow; ox++) {
      float sx = ((ox + 0.5f) * sd.box_w) / (float)ow - 0.5f;
      int x0 = (int)floorf(sx);
      float fx = sx - x0;

      uint8_t p00 = sampleSrcGlyph4bpp(sb, sd.box_w, sd.box_h, x0,     y0,     sd.bpp);
      uint8_t p10 = sampleSrcGlyph4bpp(sb, sd.box_w, sd.box_h, x0 + 1, y0,     sd.bpp);
      uint8_t p01 = sampleSrcGlyph4bpp(sb, sd.box_w, sd.box_h, x0,     y0 + 1, sd.bpp);
      uint8_t p11 = sampleSrcGlyph4bpp(sb, sd.box_w, sd.box_h, x0 + 1, y0 + 1, sd.bpp);

      float top = p00 + (p10 - p00) * fx;
      float bot = p01 + (p11 - p01) * fx;
      int val = (int)(top + (bot - top) * fy + 0.5f);
      s_scaledGlyphBuf[(uint32_t)oy * ow + ox] = (uint8_t)constrain(val, 0, 255);
    }
  }
  return s_scaledGlyphBuf;
}

void updateScaledClockFontMetrics() {
  const int num = clockScaleNum();
  s_clockFontBig.line_height = (lv_coord_t)((lv_font_montserrat_48.line_height * num + CLOCK_SCALE_DEN / 2) / CLOCK_SCALE_DEN);
  s_clockFontBig.base_line   = (lv_coord_t)((lv_font_montserrat_48.base_line * num + CLOCK_SCALE_DEN / 2) / CLOCK_SCALE_DEN);
}

void initScaledClockFont() {
  s_clockFontBig = lv_font_montserrat_48;
  s_clockFontBig.get_glyph_dsc    = scaledClockGetDsc;
  s_clockFontBig.get_glyph_bitmap = scaledClockGetBitmap;
  updateScaledClockFontMetrics();
}

const lv_font_t* getScaledClockFont() {
  return &s_clockFontBig;
}
