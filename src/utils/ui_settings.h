#pragma once

#include <lvgl.h>

struct UiSettings {
  int  scalePct;    // 60..110 (%) untuk skala konten sisi kanan
  int  rowGapPx;    // 2..24 (px) jarak antar baris konten
  bool showSeconds; // tampilkan :SS pada jam
  bool showDate;    // tampilkan hari & tanggal
};

using UiSettingsListener = void (*)(const UiSettings& cfg);

const UiSettings& uiSettingsGet();
void uiSettingsLoad();
void uiSettingsSave();
void uiSettingsReset();
void uiSettingsUpdate(const UiSettings& nextCfg, bool persist = true);

// Daftarkan callback halaman agar otomatis ikut ter-update saat Settings berubah
void uiSettingsSubscribe(UiSettingsListener listener);
void uiSettingsNotifyAll();

// Helper skala untuk dipakai oleh Home maupun menu/halaman baru di sisi kanan
int uiScalePx(int basePx);
const lv_font_t* uiPickMontserratFont(int baseSizePx);
