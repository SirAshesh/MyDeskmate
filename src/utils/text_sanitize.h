#pragma once

#include <stddef.h>
#include <stdint.h>

inline char mapLatin1ToAscii(uint32_t cp) {
  if (cp == 0x00A0) return ' ';
  if (cp >= 0x00C0 && cp <= 0x00C5) return 'A';
  if (cp == 0x00C7) return 'C';
  if (cp >= 0x00C8 && cp <= 0x00CB) return 'E';
  if (cp >= 0x00CC && cp <= 0x00CF) return 'I';
  if (cp == 0x00D1) return 'N';
  if ((cp >= 0x00D2 && cp <= 0x00D6) || cp == 0x00D8) return 'O';
  if (cp >= 0x00D9 && cp <= 0x00DC) return 'U';
  if (cp == 0x00DD) return 'Y';

  if (cp >= 0x00E0 && cp <= 0x00E5) return 'a';
  if (cp == 0x00E7) return 'c';
  if (cp >= 0x00E8 && cp <= 0x00EB) return 'e';
  if (cp >= 0x00EC && cp <= 0x00EF) return 'i';
  if (cp == 0x00F1) return 'n';
  if ((cp >= 0x00F2 && cp <= 0x00F6) || cp == 0x00F8) return 'o';
  if (cp >= 0x00F9 && cp <= 0x00FC) return 'u';
  if (cp == 0x00FD || cp == 0x00FF) return 'y';
  return 0;
}

// Normalisasi string UTF-8 agar tidak muncul glyph kotak [] pada font Montserrat ASCII LVGL
inline void sanitizeUtf8ForLvgl(const char* src, char* dst, size_t dstLen) {
  if (!dst || dstLen == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }

  const uint8_t* p = reinterpret_cast<const uint8_t*>(src);
  size_t out = 0;

  while (*p != '\0' && out + 1 < dstLen) {
    uint8_t c = *p;
    if (c < 0x80) {
      if (c >= 0x20 && c <= 0x7E) {
        dst[out++] = static_cast<char>(c);
      } else if (c == '\n' || c == '\r' || c == '\t') {
        if (out > 0 && dst[out - 1] != ' ') {
          dst[out++] = ' ';
        }
      }
      p++;
      continue;
    }

    uint32_t cp = 0;
    size_t adv = 1;
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
      cp = ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
      adv = 2;
    } else if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      adv = 3;
    } else if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
      cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
           ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
      adv = 4;
    }
    p += adv;

    if (cp == 0x00DF) {
      if (out + 2 < dstLen) {
        dst[out++] = 's';
        dst[out++] = 's';
      }
    } else if (cp >= 0x00A0 && cp <= 0x00FF) {
      char m = mapLatin1ToAscii(cp);
      if (m) dst[out++] = m;
    } else if (cp >= 0x2010 && cp <= 0x2015) {
      dst[out++] = '-';
    } else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A) {
      dst[out++] = '\'';
    } else if (cp == 0x201C || cp == 0x201D || cp == 0x201E) {
      dst[out++] = '"';
    } else if (cp == 0x2022) {
      dst[out++] = '-';
    } else if (cp == 0x2026) {
      for (int k = 0; k < 3 && out + 1 < dstLen; k++) dst[out++] = '.';
    }
    // Karakter Unicode lain (emoji, dll) dilewati agar tidak menjadi kotak []
  }

  while (out > 0 && dst[out - 1] == ' ') out--;
  dst[out] = '\0';
}
