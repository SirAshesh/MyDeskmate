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

namespace {
struct MqttNotifMsg {
  char title[48];
  char body[128];
};

QueueHandle_t    s_queue = nullptr;
bool             s_taskStarted = false;
WiFiClientSecure s_tlsClient;
PubSubClient     s_mqtt(s_tlsClient);
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
  if (!s_queue || length == 0) return;

  String raw;
  raw.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    raw += (char)payload[i];
  }
  raw.trim();

  MqttNotifMsg msg = {};
  if (raw.startsWith("{")) {
    String title = extractJsonStr(raw, "title");
    String body  = extractJsonStr(raw, "message");
    if (body.length() == 0) body = extractJsonStr(raw, "body");
    if (title.length() == 0) title = "Notifikasi";
    snprintf(msg.title, sizeof(msg.title), "%s", title.c_str());
    snprintf(msg.body, sizeof(msg.body), "%s", body.c_str());
  } else {
    snprintf(msg.title, sizeof(msg.title), "MQTT");
    snprintf(msg.body, sizeof(msg.body), "%s", raw.c_str());
  }

  xQueueSend(s_queue, &msg, 0);
}

static void mqttTask(void* /*param*/) {
  s_tlsClient.setInsecure(); // Enkripsi TLS tanpa pin sertifikat CA
  s_mqtt.setServer(MQTT_BROKER_HOST, MQTT_BROKER_PORT);
  s_mqtt.setCallback(onMqttMessage);
  s_mqtt.setBufferSize(512);

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
            s_mqtt.subscribe(MQTT_NOTIFY_TOPIC, 1);
            Serial.printf("[MQTT TLS] Connected & subscribed: %s\n", MQTT_NOTIFY_TOPIC);
          } else {
            Serial.printf("[MQTT TLS] Connect failed rc=%d\n", s_mqtt.state());
          }
        }
      } else {
        s_mqtt.loop();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void mqttNotifyLoop() {
  if (!s_taskStarted && WiFi.status() == WL_CONNECTED) {
    s_taskStarted = true;
    s_queue = xQueueCreate(8, sizeof(MqttNotifMsg));
    xTaskCreatePinnedToCore(mqttTask, "mqtt_tls", 8192, nullptr, 1, nullptr, 0);
  }

  if (s_queue) {
    MqttNotifMsg msg;
    while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) {
      char timeBuf[16] = {0};
      time_t now = time(nullptr);
      if (now >= 1700000000L) {
        struct tm tmNow = {};
        localtime_r(&now, &tmNow);
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
      }
      notificationPush(msg.title, msg.body, timeBuf, StudioTheme::CHAMPAGNE);
    }
  }
}
