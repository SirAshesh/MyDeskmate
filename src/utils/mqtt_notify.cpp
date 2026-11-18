#include "mqtt_notify.h"
#include "../ui/notification_shade.h"
#include "../ui/studio_theme.h"
#include "wifi_config.h"
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef MQTT_BROKER_HOST
#define MQTT_BROKER_HOST "broker.emqx.io"
#endif

#ifndef MQTT_BROKER_PORT
#define MQTT_BROKER_PORT 8883
#endif

#ifndef MQTT_NOTIFY_TOPIC
#define MQTT_NOTIFY_TOPIC "mydeskmate/sirashesh/notif"
#endif

#ifndef MQTT_SYNC_TOPIC
#define MQTT_SYNC_TOPIC MQTT_NOTIFY_TOPIC "/sync"
#endif

namespace {
struct MqttNotifMsg {
  bool isRemove;
  char key[96];
  char title[48];
  char body[128];
};

struct MqttTxMsg {
  char payload[160];
};

QueueHandle_t    s_rxQueue = nullptr;
QueueHandle_t    s_txQueue = nullptr;
bool             s_taskStarted = false;
WiFiClientSecure s_tlsClient;
PubSubClient     s_mqtt(s_tlsClient);
int              s_lastRc = -99;
uint32_t         s_rxCount = 0;
} // namespace

static String extractJsonStr(const String& json, const char* key) {
  String pattern = String("\"") + key + "\"";
  int idx = json.indexOf(pattern);
  if (idx < 0) return "";
  int colon = json.indexOf(':', idx + pattern.length());
  if (colon < 0) return "";
  int q1 = json.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = q1 + 1;
  while (q2 < (int)json.length()) {
    if (json[q2] == '"' && json[q2 - 1] != '\\') break;
    q2++;
  }
  if (q2 >= (int)json.length()) return "";
  String val = json.substring(q1 + 1, q2);
  val.replace("\\\"", "\"");
  val.replace("\\n", " ");
  return val;
}

static void onMqttMessage(char* /*topic*/, byte* payload, unsigned int length) {
  if (!s_rxQueue || length == 0) return;

  String raw;
  raw.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    raw += (char)payload[i];
  }
  raw.trim();

  MqttNotifMsg msg = {};
  if (raw.startsWith("{")) {
    String action = extractJsonStr(raw, "action");
    String key    = extractJsonStr(raw, "key");
    snprintf(msg.key, sizeof(msg.key), "%s", key.c_str());

    if (action == "remove") {
      msg.isRemove = true;
    } else {
      msg.isRemove = false;
      String title = extractJsonStr(raw, "title");
      String body  = extractJsonStr(raw, "message");
      if (body.length() == 0) body = extractJsonStr(raw, "body");
      if (title.length() == 0) title = "Notifikasi";
      snprintf(msg.title, sizeof(msg.title), "%s", title.c_str());
      snprintf(msg.body, sizeof(msg.body), "%s", body.c_str());
    }
  } else {
    msg.isRemove = false;
    snprintf(msg.title, sizeof(msg.title), "MQTT");
    snprintf(msg.body, sizeof(msg.body), "%s", raw.c_str());
  }

  xQueueSend(s_rxQueue, &msg, 0);
  s_rxCount++;
}

static void mqttTask(void* /*param*/) {
  s_tlsClient.setInsecure(); // Enkripsi TLS tanpa pin sertifikat CA
  s_mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  s_mqtt.setCallback(onMqttMessage);
  s_mqtt.setBufferSize(768);

  uint32_t lastReconnect = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!s_mqtt.connected()) {
        uint32_t now = millis();
        if (now - lastReconnect >= 5000) {
          lastReconnect = now;
          char clientId[40];
          snprintf(clientId, sizeof(clientId), "mydeskmate_esp32_%08lx", (unsigned long)ESP.getEfuseMac());
          Serial.printf("[MQTT TLS] Connecting to %s:%d...\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
          if (s_mqtt.connect(clientId)) {
            s_lastRc = 0;
            s_mqtt.subscribe(MQTT_NOTIFY_TOPIC, 1);
            Serial.printf("[MQTT TLS] Connected & subscribed: %s\n", MQTT_NOTIFY_TOPIC);
          } else {
            s_lastRc = s_mqtt.state();
            Serial.printf("[MQTT TLS] Connect failed rc=%d\n", s_lastRc);
          }
        }
      } else {
        s_mqtt.loop();

        // Kirim perintah sync (dismiss / clear_all) ke Android jika ada di antrean
        if (s_txQueue) {
          MqttTxMsg tx;
          while (xQueueReceive(s_txQueue, &tx, 0) == pdTRUE) {
            s_mqtt.publish(MQTT_SYNC_TOPIC, tx.payload);
          }
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void mqttNotifyLoop() {
  if (!s_taskStarted && WiFi.status() == WL_CONNECTED) {
    s_taskStarted = true;
    s_rxQueue = xQueueCreate(8, sizeof(MqttNotifMsg));
    s_txQueue = xQueueCreate(8, sizeof(MqttTxMsg));
    xTaskCreatePinnedToCore(mqttTask, "mqtt_tls", 8192, nullptr, 1, nullptr, 0);
  }

  if (s_rxQueue) {
    MqttNotifMsg msg;
    while (xQueueReceive(s_rxQueue, &msg, 0) == pdTRUE) {
      if (msg.isRemove) {
        notificationRemoveByKey(msg.key);
      } else {
        char timeBuf[16] = {0};
        time_t now = time(nullptr);
        if (now >= 1700000000L) {
          struct tm tmNow = {};
          localtime_r(&now, &tmNow);
          snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
        }
        notificationPush(msg.title, msg.body, timeBuf, StudioTheme::CHAMPAGNE, msg.key);
      }
    }
  }
}

void mqttNotifySendDismiss(const char* key) {
  if (!s_txQueue || !key || key[0] == '\0') return;
  MqttTxMsg tx = {};
  snprintf(tx.payload, sizeof(tx.payload), "{\"action\":\"remove\",\"key\":\"%s\"}", key);
  xQueueSend(s_txQueue, &tx, 0);
}

void mqttNotifySendClearAll() {
  if (!s_txQueue) return;
  MqttTxMsg tx = {};
  snprintf(tx.payload, sizeof(tx.payload), "{\"action\":\"clear_all\"}");
  xQueueSend(s_txQueue, &tx, 0);
}

void mqttNotifyGetStatus(char* buf, size_t len) {
  if (!buf || len == 0) return;
  snprintf(
    buf, len,
    "connected=%d state=%d lastRc=%d rx=%lu topic=%s broker=%s:%d freeHeap=%lu maxAlloc=%lu",
    s_mqtt.connected() ? 1 : 0,
    s_mqtt.state(),
    s_lastRc,
    (unsigned long)s_rxCount,
    MQTT_NOTIFY_TOPIC,
    MQTT_BROKER_HOST,
    MQTT_BROKER_PORT,
    (unsigned long)ESP.getFreeHeap(),
    (unsigned long)ESP.getMaxAllocHeap()
  );
}
