#pragma once

// ============================================================
// KONFIGURASI TAMPILAN UI (480x320 LANDSCAPE)
// ============================================================

// Orientasi layar HX8357D:
//   1 = Landscape normal
//   3 = Landscape terbalik 180 derajat (atas <-> bawah, kiri <-> kanan)
//   5 = Landscape mirror (alternatif panel)
//   7 = Landscape mirror (alternatif panel)
#define UI_DISPLAY_ROTATION 3

// Lebar panel sisi kiri (khusus tempat icon), dalam piksel
#define UI_LEFT_SIDEBAR_W 68

// Debugging posisi touch di layar (overlay titik + koordinat pada lv_layer_top):
//   1 = Aktif (tampilkan titik sentuh & koordinat X/Y)
//   0 = Nonaktif
#define UI_DEBUG_TOUCH_POS 1

// Durasi idle (ms) sebelum masuk mode layar gelap + jam + pet (default: 5 menit)
#define UI_IDLE_TIMEOUT_MS (5UL * 60UL * 1000UL)
