# MyDeskmate ESP32-S3 + TFT 3.5" Touchscreen Simulator

Simulator hardware-level untuk **ESP32-S3** (dual-core Xtensa LX7 asli + mask ROM Espressif `esp32s3_rev0_rom.elf` + bootloader + FreeRTOS + Arduino-ESP32 + 8 MB Flash + 8 MB OPI PSRAM) yang terhubung ke layar **TFT 3.5" HX8357D 8-bit parallel (320×480)** dan **4-wire resistive touchscreen** sesuai mapping pin pada [`pin.txt`](../pin.txt).

> **Catatan Arsitektur CPU:** CPU utama ESP32-S3 adalah **dual-core Xtensa LX7** (bukan RISC-V — pada ESP32-S3 hanya koprosesor ULP yang RISC-V; keluarga RISC-V ada di ESP32-C3/C6/H2/P4). Karena itu simulator ini mengeksekusi instruksi **Xtensa LX7 asli ESP32-S3** agar bisa menjalankan firmware `.pio/build/esp32-s3-devkitc-1/firmware.bin` secara langsung tanpa modifikasi.

## Mapping Pin Akurat (`pin.txt` & `src/hal/touch_4wire.cpp`)

| Sinyal | GPIO ESP32-S3 | Keterangan |
|---|---|---|
| `LCD_RST` | GPIO 4 | Active-low hardware reset HX8357D |
| `LCD_CS` | GPIO 5 | Active-low chip select |
| `LCD_RS` / `TS_XM` | GPIO 6 | D/C (`LCD_DC_IDX` 153) & Touch X- (`ADC1_CH5`) |
| `LCD_WR` / `TS_YP` | GPIO 7 | Write strobe (`LCD_PCLK_IDX` 154) & Touch Y+ (`ADC1_CH6`) |
| `LCD_RD` | GPIO 15 | Read strobe |
| `LCD_D0..D5` | GPIO 16, 17, 18, 8, 9, 10 | `LCD_DATA_OUT0..5` (133..138) + GPIO bit-bang |
| `LCD_D6` / `TS_XP` | GPIO 11 | `LCD_DATA_OUT6` (139) & Touch X+ |
| `LCD_D7` / `TS_YM` | GPIO 13 | `LCD_DATA_OUT7` (140) & Touch Y- |

## Cara Menjalankan

```powershell
# Jalankan langsung (tampilan polos hanya TFT 480x320, skala 2x = 960x640)
.\simulation\run.ps1

# Atau klik ganda:
# simulation\run.bat
```

Opsi tambahan:
- `-Borderless` — jendela tanpa border/titlebar sama sekali (klik-kanan drag untuk menggeser, `Esc` untuk menutup)
- `-Scale 1|2|3` — skala jendela TFT (default `2`)
- `-RebuildFirmware` — compile ulang firmware PlatformIO sebelum menjalankan simulator
- `-Serial` — tampilkan log UART0 ESP32-S3 di terminal
