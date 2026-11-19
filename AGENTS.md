# Panduan Pengembangan Aplikasi & OS MyDeskmate (ESP32-S3 + LVGL 8.4)

Dokumen ini menjelaskan arsitektur **MyDeskmate OS (`A + B`)**, struktur folder `src/`, serta cara membuat aplikasi baru di `src/app/` dan mendaftarkannya ke OS (`480x320` landscape, HX8357D + LVGL 8.4 + 4-wire resistive touch).

---

## 1. Struktur Folder Proyek

- **`include/`**:
  - `ui_config.h`: Orientasi layar (`UI_DISPLAY_ROTATION`) dan lebar sidebar kiri (`UI_LEFT_SIDEBAR_W = 68`).
  - `lv_conf.h`: Konfigurasi LVGL (`LV_MEM_SIZE = 128 KB`, font Montserrat `12,14,16,20,24,28,48`, `LV_USE_CANVAS`, `LV_USE_QRCODE`).
  - `wifi_config.h`: Kredensial WiFi, server quota, dan host bridge VPS (`LYRICS_WS_HOST`, `LYRICS_WS_PORT`).
- **`src/hal/`**: Driver layar (`display_hx8357d.*`) dan 4-wire resistive touch (`touch_4wire.*` dengan debounce & smoothing).
- **`src/os/`**: Inti OS (*App Lifecycle*, *Crash Guard*, *Shell*, *Boot Screen*, *Notification Shade*, *StudioTheme*):
  - `app_interface.h`: Kontrak `AppDescriptor`.
  - `app_manager.*`: Lazy-load app (`onCreate`/`onDestroy`), `onBackgroundTick`, dan **Crash Guard** berbasis NVS (`os_guard`) + `esp_reset_reason()`.
  - `ui_shell.*`: Membuat `panelLeft` (sidebar scrollable, scrollbar tersembunyi), `divider`, `panelRight` (`411x320`), dan mendaftarkan seluruh aplikasi.
  - `studio_theme.h`: Palet warna `StudioTheme` dan helper `makePlainBox()`.
- **`src/app/`**: Setiap aplikasi berada dalam foldernya sendiri:
  - `home/` — Jam & tanggal (`keepAlive = true`)
  - `antigravity/` — Monitor kuota Gemini/Antigravity
  - `discord/` — Mini Discord Client (read-only via bridge VPS terenkripsi)
  - `music/` — Pemutar & lirik YouTube tersinkron
  - `timer/` — Hitung mundur (tetap berjalan di background via `onBackgroundTick`)
  - `system_manager/` — Monitor RAM/PSRAM/Flash/WiFi/MQTT & status Crash Guard
  - `settings/` — Pengaturan skala UI, jam, debug sentuhan, dan pemulihan karantina
- **`src/utils/`**: Utilitas bersama (`mqtt_notify.*`, `notify_server.*`, `scaled_font.*`, `text_sanitize.h`, `ui_settings.*`).

---

## 2. Kontrak Aplikasi (`AppDescriptor`)

Setiap app wajib mengembalikan `AppDescriptor` (didefinisikan di `src/os/app_interface.h`):

```cpp
struct AppDescriptor {
  const char*         id;               // ID unik untuk NVS Crash Guard (mis. "notes")
  const char*         name;             // Nama tampil di UI / notifikasi
  const char*         symbol;           // Ikon LV_SYMBOL_* (nullptr jika pakai iconImg)
  const lv_img_dsc_t* iconImg;          // Opsional ikon C array
  bool                keepAlive;        // false = lazy-load & dihancurkan saat keluar (hemat RAM)
  lv_obj_t*         (*onCreate)(lv_obj_t* parent, int w, int h);
  void              (*onDestroy)();     // Wajib hapus lv_timer & null-kan pointer widget
  void              (*onResume)();      // Opsional: dipanggil saat app tampil
  void              (*onPause)();       // Opsional: dipanggil saat keluar dari app
  void              (*onBackgroundTick)(); // Opsional: dipanggil di loop() meski UI ditutup
};
```

---

## 3. Cara Membuat Aplikasi Baru di `src/app/`

### Langkah 1: Buat Folder & Header (`src/app/<nama>/app_<nama>.h`)
```cpp
#pragma once
#include "../../os/app_interface.h"

AppDescriptor appNotesDescriptor();
```

### Langkah 2: Implementasi (`src/app/<nama>/app_<nama>.cpp`)
```cpp
#include "app_notes.h"
#include "../../os/studio_theme.h"

static lv_obj_t*   s_page  = nullptr;
static lv_timer_t* s_timer = nullptr;

static lv_obj_t* notesOnCreate(lv_obj_t* parent, int rightW, int rightH) {
  s_page = makePlainBox(parent, 0, 0, rightW, rightH);
  lv_obj_set_style_pad_all(s_page, 14, 0);

  lv_obj_t* lbl = lv_label_create(s_page);
  lv_label_set_text(lbl, "Notes App");
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(StudioTheme::TEXT), 0);
  lv_obj_center(lbl);

  return s_page;
}

static void notesOnDestroy() {
  if (s_timer) {
    lv_timer_del(s_timer);
    s_timer = nullptr;
  }
  // Jangan panggil lv_obj_del(s_page) di sini — OS menghapus root page secara otomatis
  s_page = nullptr;
}

AppDescriptor appNotesDescriptor() {
  return AppDescriptor{
    "notes",
    "Notes",
    LV_SYMBOL_EDIT,
    nullptr,
    false,          // Lazy-load & hancurkan UI saat pindah menu
    notesOnCreate,
    notesOnDestroy,
    nullptr,
    nullptr,
    nullptr
  };
}
```

### Langkah 3: Daftarkan ke OS (`src/os/ui_shell.cpp`)
Include header app baru lalu tambahkan ke urutan menu di `uiShellInit()` (maksimal `OS_MAX_APPS = 12` di `app_manager.h`):

```cpp
#include "../app/notes/app_notes.h"
...
appManagerRegister(appNotesDescriptor());
```

---

## 4. Aturan Penting Memori, Task & Stabilitas (ESP32-S3)

1. **Jaga RAM Internal & Stack:**
   - **Jangan** menaruh array/struct besar (> 512 byte) di stack fungsi UI (`loopTask` hanya 8 KB) atau sebagai array statis besar di RAM internal (dibutuhkan WiFi/DMA).
   - Gunakan **PSRAM** (`heap_caps_calloc(..., MALLOC_CAP_SPIRAM)`) untuk buffer besar (seperti gambar RGB565 atau daftar item panjang).
2. **Thread-Safety LVGL & Dual Core:**
   - Tugas jaringan (HTTP/WebSocket) jalankan di **Core 0** (`xTaskCreatePinnedToCore(..., 0)`), hanya buat task tersebut **sekali** (`static bool s_taskStarted`), lalu salin data ke UI lewat `SemaphoreHandle_t` / queue.
   - **Jangan pernah** memanggil fungsi LVGL dari task Core 0; gunakan `lv_timer` di UI thread (Core 1).
3. **Pembersihan di `onDestroy()`:**
   - Wajib memanggil `lv_timer_del()` untuk semua `lv_timer_t*` milik app dan me-reset semua pointer `lv_obj_t*` ke `nullptr` agar tidak terjadi *dangling pointer* atau *memory leak*.
4. **Crash Guard (Otomatis):**
   - Jika app menyebabkan `PANIC` atau `WDT` saat aktif, OS otomatis mengkarantina app tersebut pada boot berikutnya dan tetap masuk ke `Home`. Pemulihan dapat dilakukan lewat tombol **"Pulihkan & Buka"** atau menu **Settings → Pulihkan**.
5. **Catatan Font & Teks LVGL 8.4:**
   - Jangan gunakan `lv_obj_set_style_transform_zoom` pada `lv_label`.
   - Sanitasi teks eksternal (nama lagu, pesan, notifikasi) memakai `sanitizeUtf8ForLvgl()` di `src/utils/text_sanitize.h`.

---

## 5. Build & Flash ke ESP32-S3

Setiap perubahan firmware wajib di-compile dan di-flash ke board ESP32-S3 (`COM9` sesuai `platformio.ini`):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t upload
```
