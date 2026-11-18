#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <nvs_flash.h>

#include "hal/display_hx8357d.h"
#include "hal/touch_4wire.h"
#include "ui/page_antigravity.h"
#include "ui/ui_manager.h"

static lv_disp_draw_buf_t lvDrawBuf;
static lv_color_t*        lvBuf1 = nullptr;
static constexpr int      LV_BUF_LINES = 32;
static lv_disp_drv_t      lvDispDrv;
static lv_indev_drv_t     lvIndevDrv;

static void handleSerialCommands() {
  static bool invertFlipped = false;
  static char lineBuf[384];
  static int  lineLen = 0;
  static bool readingLine = false;

  while (Serial.available() > 0) {
    char c = (char)Serial.read();

    if (readingLine) {
      if (c == '\n' || c == '\r') {
        if (lineLen > 0) {
          lineBuf[lineLen] = '\0';
          antigravityHandleSerialLine(lineBuf);
          lineLen = 0;
        }
        readingLine = false;
      } else if (lineLen < (int)sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      }
      continue;
    }

    if (c == 'Q') {
      readingLine = true;
      lineLen = 0;
      lineBuf[lineLen++] = c;
      continue;
    }

    if (c == 'i' || c == 'I') {
      invertFlipped = !invertFlipped;
      tft.invertDisplay(invertFlipped);
      Serial.printf("[LCD] invertDisplay(%d)\n", invertFlipped);
    } else if (c == 'c' || c == 'C') {
      runTouchCalibration();
    } else if (c == 't' || c == 'T') {
      int rx = 0, ry = 0, pz = 0;
      bool pressed = readTouchRaw(rx, ry, pz);
      Serial.printf("[TOUCH TEST] pressed=%d z=%d rawX=%d rawY=%d\n", pressed, pz, rx, ry);
    } else if (c == 'r' || c == 'R') {
      resetTouchCal();
      Serial.println("[CAL] Touch calibration reset to default");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.println("[NVS] Erasing & re-initializing NVS...");
    nvs_flash_erase();
    nvsErr = nvs_flash_init();
  }
  if (nvsErr != ESP_OK) {
    Serial.printf("[NVS] nvs_flash_init failed: 0x%x\n", nvsErr);
  }

  loadTouchCal();
  displayInit();

  lv_init();
  lvBuf1 = (lv_color_t*)heap_caps_malloc(UI_W * LV_BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA);
  if (!lvBuf1) {
    lvBuf1 = (lv_color_t*)malloc(UI_W * LV_BUF_LINES * sizeof(lv_color_t));
  }
  lv_disp_draw_buf_init(&lvDrawBuf, lvBuf1, nullptr, UI_W * LV_BUF_LINES);

  lv_disp_drv_init(&lvDispDrv);
  lvDispDrv.hor_res  = UI_W;
  lvDispDrv.ver_res  = UI_H;
  lvDispDrv.flush_cb = lvglFlushCb;
  lvDispDrv.draw_buf = &lvDrawBuf;
  lv_disp_drv_register(&lvDispDrv);

  lv_indev_drv_init(&lvIndevDrv);
  lvIndevDrv.type    = LV_INDEV_TYPE_POINTER;
  lvIndevDrv.read_cb = lvglTouchReadCb;
  lv_indev_drv_register(&lvIndevDrv);

  buildUi();

  Serial.println("==========================================");
  Serial.println("  S3-OS Studio UI (LVGL + LovyanGFX)");
  Serial.println("  Serial cmd: [c] Cal | [t] Touch | [r] Reset Cal | [i] Invert");
  Serial.println("==========================================");
}

void loop() {
  handleSerialCommands();
  lv_timer_handler();
  delay(5);
}
