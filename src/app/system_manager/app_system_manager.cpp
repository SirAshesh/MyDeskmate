#include "app_system_manager.h"
#include "../../os/app_manager.h"
#include "../../os/studio_theme.h"
#include "../../utils/mqtt_notify.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <stdio.h>

static lv_obj_t* s_pageResources = nullptr;
static lv_obj_t* s_lblChipMeta   = nullptr;
static lv_obj_t* s_lblHeapVal    = nullptr;
static lv_obj_t* s_barHeap       = nullptr;
static lv_obj_t* s_lblPsramVal   = nullptr;
static lv_obj_t* s_barPsram      = nullptr;
static lv_obj_t* s_lblFlashMeta  = nullptr;
static lv_obj_t* s_lblSketchVal  = nullptr;
static lv_obj_t* s_barSketch     = nullptr;
static lv_obj_t* s_lblWifiVal    = nullptr;
static lv_obj_t* s_lblMqttVal    = nullptr;
static lv_obj_t*   s_lblSysVal     = nullptr;
static lv_timer_t* s_resTimer      = nullptr;

static lv_obj_t* makeResourceCard(lv_obj_t* parent, int w) {
  lv_obj_t* card = makePlainBox(parent, 0, 0, w, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(card, lv_color_hex(StudioTheme::SURFACE), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(StudioTheme::RULE), 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_radius(card, 10, 0);
  lv_obj_set_style_pad_hor(card, 12, 0);
  lv_obj_set_style_pad_ver(card, 8, 0);
  lv_obj_set_style_pad_row(card, 5, 0);
  lv_obj_set_layout(card, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  return card;
}

static lv_obj_t* makeRow(lv_obj_t* parent) {
  lv_obj_t* row = makePlainBox(parent, 0, 0, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_layout(row, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  return row;
}

static lv_obj_t* makeUsageBar(lv_obj_t* parent, uint32_t accentHex) {
  lv_obj_t* bar = lv_bar_create(parent);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, lv_pct(100), 8);
  lv_bar_set_range(bar, 0, 100);
  lv_obj_set_style_bg_color(bar, lv_color_hex(StudioTheme::TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);

  lv_obj_set_style_bg_color(bar, lv_color_hex(accentHex), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
  return bar;
}

static uint32_t pickUsageColor(int pct, uint32_t normalHex) {
  if (pct >= 85) return StudioTheme::ROSE;
  if (pct >= 70) return StudioTheme::CORAL;
  return normalHex;
}

static void updateResourcesDisplay(lv_timer_t* timer) {
  (void)timer;
  if (!s_pageResources) return;
  // Lewati update saat halaman tersembunyi agar hemat CPU
  if (timer && lv_obj_has_flag(s_pageResources, LV_OBJ_FLAG_HIDDEN)) return;

  char buf[96];

  // Uptime & CPU freq
  uint32_t upSec = millis() / 1000UL;
  uint32_t upH = upSec / 3600UL;
  uint32_t upM = (upSec % 3600UL) / 60UL;
  uint32_t upS = upSec % 60UL;
  snprintf(buf, sizeof(buf), "%u MHz  •  up %02lu:%02lu:%02lu",
           (unsigned)ESP.getCpuFreqMHz(), (unsigned long)upH, (unsigned long)upM, (unsigned long)upS);
  lv_label_set_text(s_lblChipMeta, buf);

  // Internal Heap
  uint32_t heapTotal = ESP.getHeapSize();
  uint32_t heapFree  = ESP.getFreeHeap();
  uint32_t heapUsed  = (heapTotal > heapFree) ? (heapTotal - heapFree) : 0;
  int heapPct = (heapTotal > 0) ? (int)((heapUsed * 100ULL) / heapTotal) : 0;
  snprintf(buf, sizeof(buf), "%u / %u KB (%d%%) • min %u KB",
           (unsigned)(heapUsed / 1024), (unsigned)(heapTotal / 1024),
           heapPct, (unsigned)(ESP.getMinFreeHeap() / 1024));
  lv_label_set_text(s_lblHeapVal, buf);
  lv_bar_set_value(s_barHeap, heapPct, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s_barHeap, lv_color_hex(pickUsageColor(heapPct, StudioTheme::TEAL)), LV_PART_INDICATOR);

  // PSRAM
  uint32_t psTotal = ESP.getPsramSize();
  if (psTotal > 0) {
    uint32_t psFree = ESP.getFreePsram();
    uint32_t psUsed = (psTotal > psFree) ? (psTotal - psFree) : 0;
    int psPct = (int)((psUsed * 100ULL) / psTotal);
    snprintf(buf, sizeof(buf), "%u / %u KB (%d%%)",
             (unsigned)(psUsed / 1024), (unsigned)(psTotal / 1024), psPct);
    lv_label_set_text(s_lblPsramVal, buf);
    lv_bar_set_value(s_barPsram, psPct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_barPsram, lv_color_hex(pickUsageColor(psPct, StudioTheme::CHAMPAGNE)), LV_PART_INDICATOR);
  } else {
    lv_label_set_text(s_lblPsramVal, "Tidak tersedia");
    lv_bar_set_value(s_barPsram, 0, LV_ANIM_OFF);
  }

  // Flash & Sketch Partition
  uint32_t flashMB = ESP.getFlashChipSize() / (1024U * 1024U);
  uint32_t flashMHz = ESP.getFlashChipSpeed() / 1000000U;
  snprintf(buf, sizeof(buf), "Flash %u MB @ %u MHz", (unsigned)flashMB, (unsigned)flashMHz);
  lv_label_set_text(s_lblFlashMeta, buf);

  uint32_t sketchUsed  = ESP.getSketchSize();
  uint32_t sketchFree  = ESP.getFreeSketchSpace();
  uint32_t sketchTotal = sketchUsed + sketchFree;
  int sketchPct = (sketchTotal > 0) ? (int)((sketchUsed * 100ULL) / sketchTotal) : 0;
  snprintf(buf, sizeof(buf), "%u / %u KB (%d%%)",
           (unsigned)(sketchUsed / 1024), (unsigned)(sketchTotal / 1024), sketchPct);
  lv_label_set_text(s_lblSketchVal, buf);
  lv_bar_set_value(s_barSketch, sketchPct, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(s_barSketch, lv_color_hex(pickUsageColor(sketchPct, StudioTheme::SAGE)), LV_PART_INDICATOR);

  // WiFi
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(buf, sizeof(buf), "%s  •  %s  (%d dBm)",
             WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    lv_label_set_text(s_lblWifiVal, buf);
    lv_obj_set_style_text_color(s_lblWifiVal, lv_color_hex(StudioTheme::TEAL), 0);
  } else {
    lv_label_set_text(s_lblWifiVal, "Terputus / Offline");
    lv_obj_set_style_text_color(s_lblWifiVal, lv_color_hex(StudioTheme::MUTED), 0);
  }

  // MQTT
  char mqttBuf[64];
  mqttNotifyGetStatus(mqttBuf, sizeof(mqttBuf));
  lv_label_set_text(s_lblMqttVal, mqttBuf);
  bool mqttOk = (strstr(mqttBuf, "Connected") != nullptr || strstr(mqttBuf, "connected") != nullptr || strstr(mqttBuf, "OK") != nullptr);
  lv_obj_set_style_text_color(s_lblMqttVal, lv_color_hex(mqttOk ? StudioTheme::SAGE : StudioTheme::CHAMPAGNE), 0);

  // System Temp, Chip & Crash Guard
  float tempC = temperatureRead();
  snprintf(buf, sizeof(buf), "%s r%d  •  %.1f°C  •  Q:%d",
           ESP.getChipModel(), ESP.getChipRevision(), tempC, appManagerGetQuarantineCount());
  lv_label_set_text(s_lblSysVal, buf);
}

static lv_obj_t* systemManagerOnCreate(lv_obj_t* parent, int rightW, int rightH) {
  s_pageResources = makePlainBox(parent, 0, 0, rightW, rightH);
  lv_obj_set_style_pad_hor(s_pageResources, 16, 0);
  lv_obj_set_style_pad_ver(s_pageResources, 10, 0);
  lv_obj_set_style_pad_row(s_pageResources, 8, 0);
  lv_obj_set_layout(s_pageResources, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_pageResources, LV_FLEX_FLOW_COLUMN);

  const int cardW = rightW - 32;

  // Header
  lv_obj_t* hdr = makePlainBox(s_pageResources, 0, 0, cardW, LV_SIZE_CONTENT);
  lv_obj_set_layout(hdr, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t* title = lv_label_create(hdr);
  lv_label_set_text(title, "Resource Manager");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(StudioTheme::TEXT), 0);

  s_lblChipMeta = lv_label_create(hdr);
  lv_label_set_text(s_lblChipMeta, "-- MHz");
  lv_obj_set_style_text_font(s_lblChipMeta, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblChipMeta, lv_color_hex(StudioTheme::MUTED), 0);

  // Card 1: Memory (Heap + PSRAM)
  lv_obj_t* cardMem = makeResourceCard(s_pageResources, cardW);

  lv_obj_t* rowHeap = makeRow(cardMem);
  lv_obj_t* lblHeapTitle = lv_label_create(rowHeap);
  lv_label_set_text(lblHeapTitle, "SRAM Heap");
  lv_obj_set_style_text_font(lblHeapTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblHeapTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_lblHeapVal = lv_label_create(rowHeap);
  lv_label_set_text(s_lblHeapVal, "-");
  lv_obj_set_style_text_font(s_lblHeapVal, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblHeapVal, lv_color_hex(StudioTheme::TEAL), 0);

  s_barHeap = makeUsageBar(cardMem, StudioTheme::TEAL);

  lv_obj_t* rowPsram = makeRow(cardMem);
  lv_obj_t* lblPsramTitle = lv_label_create(rowPsram);
  lv_label_set_text(lblPsramTitle, "PSRAM");
  lv_obj_set_style_text_font(lblPsramTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblPsramTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_lblPsramVal = lv_label_create(rowPsram);
  lv_label_set_text(s_lblPsramVal, "-");
  lv_obj_set_style_text_font(s_lblPsramVal, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblPsramVal, lv_color_hex(StudioTheme::CHAMPAGNE), 0);

  s_barPsram = makeUsageBar(cardMem, StudioTheme::CHAMPAGNE);

  // Card 2: Storage (Sketch & Flash)
  lv_obj_t* cardFlash = makeResourceCard(s_pageResources, cardW);

  lv_obj_t* rowFlashHdr = makeRow(cardFlash);
  lv_obj_t* lblSketchTitle = lv_label_create(rowFlashHdr);
  lv_label_set_text(lblSketchTitle, "App Partition");
  lv_obj_set_style_text_font(lblSketchTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lblSketchTitle, lv_color_hex(StudioTheme::TEXT), 0);

  s_lblFlashMeta = lv_label_create(rowFlashHdr);
  lv_label_set_text(s_lblFlashMeta, "Flash - MB");
  lv_obj_set_style_text_font(s_lblFlashMeta, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblFlashMeta, lv_color_hex(StudioTheme::MUTED), 0);

  lv_obj_t* rowSketchVal = makeRow(cardFlash);
  lv_obj_t* lblSketchSub = lv_label_create(rowSketchVal);
  lv_label_set_text(lblSketchSub, "Firmware");
  lv_obj_set_style_text_font(lblSketchSub, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblSketchSub, lv_color_hex(StudioTheme::MUTED), 0);

  s_lblSketchVal = lv_label_create(rowSketchVal);
  lv_label_set_text(s_lblSketchVal, "-");
  lv_obj_set_style_text_font(s_lblSketchVal, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblSketchVal, lv_color_hex(StudioTheme::SAGE), 0);

  s_barSketch = makeUsageBar(cardFlash, StudioTheme::SAGE);

  // Card 3: Network & System
  lv_obj_t* cardNet = makeResourceCard(s_pageResources, cardW);

  lv_obj_t* rowWifi = makeRow(cardNet);
  lv_obj_t* lblWifiKey = lv_label_create(rowWifi);
  lv_label_set_text(lblWifiKey, "WiFi");
  lv_obj_set_style_text_font(lblWifiKey, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblWifiKey, lv_color_hex(StudioTheme::MUTED), 0);

  s_lblWifiVal = lv_label_create(rowWifi);
  lv_label_set_text(s_lblWifiVal, "-");
  lv_obj_set_style_text_font(s_lblWifiVal, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblWifiVal, lv_color_hex(StudioTheme::TEXT), 0);

  lv_obj_t* rowMqtt = makeRow(cardNet);
  lv_obj_t* lblMqttKey = lv_label_create(rowMqtt);
  lv_label_set_text(lblMqttKey, "MQTT");
  lv_obj_set_style_text_font(lblMqttKey, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblMqttKey, lv_color_hex(StudioTheme::MUTED), 0);

  s_lblMqttVal = lv_label_create(rowMqtt);
  lv_label_set_text(s_lblMqttVal, "-");
  lv_obj_set_style_text_font(s_lblMqttVal, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblMqttVal, lv_color_hex(StudioTheme::CHAMPAGNE), 0);

  lv_obj_t* rowSys = makeRow(cardNet);
  lv_obj_t* lblSysKey = lv_label_create(rowSys);
  lv_label_set_text(lblSysKey, "Chip");
  lv_obj_set_style_text_font(lblSysKey, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(lblSysKey, lv_color_hex(StudioTheme::MUTED), 0);

  s_lblSysVal = lv_label_create(rowSys);
  lv_label_set_text(s_lblSysVal, "-");
  lv_obj_set_style_text_font(s_lblSysVal, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(s_lblSysVal, lv_color_hex(StudioTheme::TEXT), 0);

  updateResourcesDisplay(nullptr);
  s_resTimer = lv_timer_create(updateResourcesDisplay, 1000, nullptr);

  return s_pageResources;
}

static void systemManagerOnDestroy() {
  if (s_resTimer) {
    lv_timer_del(s_resTimer);
    s_resTimer = nullptr;
  }
  s_pageResources = nullptr;
  s_lblChipMeta   = nullptr;
  s_lblHeapVal    = nullptr;
  s_barHeap       = nullptr;
  s_lblPsramVal   = nullptr;
  s_barPsram      = nullptr;
  s_lblFlashMeta  = nullptr;
  s_lblSketchVal  = nullptr;
  s_barSketch     = nullptr;
  s_lblWifiVal    = nullptr;
  s_lblMqttVal    = nullptr;
  s_lblSysVal     = nullptr;
}

AppDescriptor appSystemManagerDescriptor() {
  return AppDescriptor{
    "sys_mgr",
    "System Manager",
    LV_SYMBOL_DRIVE,
    nullptr,
    false,
    systemManagerOnCreate,
    systemManagerOnDestroy,
    nullptr,
    nullptr,
    nullptr
  };
}
