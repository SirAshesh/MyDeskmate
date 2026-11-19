#pragma once

#include <lvgl.h>

/**
 * Kontrak antarmuka aplikasi untuk MyDeskmate OS.
 * Memisahkan siklus hidup UI (onCreate/onDestroy) dan tugas latar belakang (onBackgroundTick).
 */
struct AppDescriptor {
  const char*         id;               // ID unik untuk NVS Crash Guard (mis. "home", "music")
  const char*         name;             // Nama tampil (mis. "Musik & Lirik")
  const char*         symbol;           // Ikon LV_SYMBOL_* (nullptr jika pakai iconImg)
  const lv_img_dsc_t* iconImg;          // Opsional ikon gambar
  bool                keepAlive;        // Jika true, UI tetap dipertahankan di memori saat pindah menu
  lv_obj_t*         (*onCreate)(lv_obj_t* parent, int w, int h);
  void              (*onDestroy)();     // Bersihkan timer UI & null-kan pointer widget (root dihapus OS)
  void              (*onResume)();      // Opsional: dipanggil saat app tampil
  void              (*onPause)();       // Opsional: dipanggil saat berpindah dari app ini
  void              (*onBackgroundTick)(); // Opsional: dipanggil tiap loop meski UI tidak aktif
};
