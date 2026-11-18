# Panduan Pengembangan UI ESP32-S3 TFT (LVGL + LovyanGFX)

Dokumen ini menjelaskan konvensi dan cara menambahkan **icon/button/menu** di sidebar kiri serta **isi container** di area konten kanan pada project ini (`480x320` landscape, HX8357D + LVGL 8.4).

---

## 1. File Utama & Konfigurasi

- **`include/ui_config.h`**:
  - `UI_DISPLAY_ROTATION`: Mengatur orientasi layar (`1` = landscape normal, `3` = landscape terbalik 180°, `5`/`7` = mirror). Kalibrasi touch di `src/main.cpp` otomatis menyesuaikan nilai ini.
  - `UI_LEFT_SIDEBAR_W`: Lebar sidebar kiri dalam piksel (saat ini `68`).
- **`include/lv_conf.h`**:
  - Mengaktifkan font Montserrat yang dipakai UI (`LV_FONT_MONTSERRAT_12`, `14`, `16`, `20`, `24`, `28`, `48`). Aktifkan font baru di sini sebelum memakainya di `src/main.cpp`.
- **`src/main.cpp`**:
  - Berisi driver panel/touch, palet warna `StudioTheme`, dan fungsi `buildUi()`.

---

## 2. Struktur Layout 2 Sisi

Pada `buildUi()`, layar (`scr`) dibagi menjadi dua container utama:

1. **Sisi Kiri (`panelLeft`)**: `x = 0, y = 0, w = UI_LEFT_SIDEBAR_W, h = UI_H` (`68 x 320`), background `StudioTheme::HEADER`. Khusus untuk tombol icon menu.
2. **Garis Pemisah (`divider`)**: `x = UI_LEFT_SIDEBAR_W, y = 0, w = 1, h = UI_H`, warna `StudioTheme::RULE`.
3. **Sisi Kanan (`panelRight`)**: `x = UI_LEFT_SIDEBAR_W + 1, y = 0, w = UI_W - UI_LEFT_SIDEBAR_W - 1, h = UI_H` (`411 x 320`), background `StudioTheme::BG`. Tempat seluruh halaman/konten utama.

---

## 3. Cara Membuat Icon / Button / Menu (`panelLeft`)

Gunakan `lv_btn_create(panelLeft)` dengan gaya bersih (`lv_obj_remove_style_all`) agar konsisten dengan tema `StudioTheme`:

```cpp
constexpr int BTN_SIZE = 50;
// Posisi Y untuk item ke-i: 12 + i * (BTN_SIZE + 10)
lv_obj_t* btn = lv_btn_create(panelLeft);
lv_obj_remove_style_all(btn);
lv_obj_set_pos(btn, (UI_LEFT_SIDEBAR_W - BTN_SIZE) / 2, 12);
lv_obj_set_size(btn, BTN_SIZE, BTN_SIZE);
lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::ELEVATED), 0);
lv_obj_set_style_bg_color(btn, lv_color_hex(StudioTheme::SURFACE), LV_STATE_PRESSED);
lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
lv_obj_set_style_border_color(btn, lv_color_hex(StudioTheme::CHAMPAGNE), 0); // atau StudioTheme::RULE jika non-aktif
lv_obj_set_style_border_width(btn, 1, 0);
lv_obj_set_style_radius(btn, 12, 0);
lv_obj_add_event_cb(btn, onMenuClick, LV_EVENT_CLICKED, nullptr);

lv_obj_t* icon = lv_label_create(btn);
lv_label_set_text(icon, LV_SYMBOL_HOME); // gunakan LV_SYMBOL_* bawaan LVGL
lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
lv_obj_set_style_text_color(icon, lv_color_hex(StudioTheme::CHAMPAGNE), 0);
lv_obj_center(icon);
```

**Tips multi-menu:**
- Buat helper untuk mengganti halaman aktif di `panelRight`: sembunyikan semua `page*` dengan `lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN)` lalu tampilkan halaman terpilih dengan `lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN)`.
- Ubah warna border dan warna icon tombol aktif menjadi `StudioTheme::CHAMPAGNE` (atau `TEAL`), dan tombol non-aktif menjadi `StudioTheme::RULE` / `MUTED`.

---

## 4. Cara Membuat Isi Container (`panelRight`)

Setiap halaman di sisi kanan dibuat sebagai container penuh di dalam `panelRight` menggunakan `makePlainBox`:

```cpp
const int rightW = UI_W - UI_LEFT_SIDEBAR_W - 1;
lv_obj_t* page = makePlainBox(panelRight, 0, 0, rightW, UI_H);
```

### Posisi Rata Tengah (Flex Column)
Untuk menaruh elemen tepat di tengah container (seperti jam, hari, dan tanggal pada `pageHome`):

```cpp
lv_obj_set_layout(page, LV_LAYOUT_FLEX);
lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
lv_obj_set_style_pad_row(page, 8, 0);
```

### Catatan Font & Ukuran Teks (LVGL 8.4)
- **Jangan gunakan `lv_obj_set_style_transform_zoom` pada `lv_label`**: pada LVGL 8.4 hal ini dapat menyebabkan label tidak ter-render / hilang.
- Jika membutuhkan ukuran font lebih besar dari `lv_font_montserrat_48`, gunakan wrapper `lv_font_t` (seperti `s_clockFontBig` di `src/main.cpp` yang meng-upscale `lv_font_montserrat_48` sebesar `1.75x` dengan bilinear interpolation ke bitmap 8bpp).

---

## 5. Build & Flash ke ESP32-S3

Setiap perubahan kode harus di-compile dan di-flash ke board ESP32-S3 (`COM9` sesuai `platformio.ini`):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -t upload
```
