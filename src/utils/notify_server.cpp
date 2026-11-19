#include "notify_server.h"
#include "mqtt_notify.h"
#include "../os/notification_shade.h"
#include "../os/studio_theme.h"
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace {
WebServer s_server(80);
bool s_started = false;
} // namespace

static void formatNowWita(char* out, size_t len) {
  time_t now = time(nullptr);
  if (now < 1700000000L) {
    out[0] = '\0';
    return;
  }
  struct tm tmNow = {};
  localtime_r(&now, &tmNow);
  snprintf(out, len, "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
}

// Ekstrak nilai string sederhana dari JSON datar: {"key":"value",...}
static String extractJsonField(const String& json, const char* key) {
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

static void handleRoot() {
  String msg = "MyDeskmate Notify Server OK\nIP: " + WiFi.localIP().toString() +
               "\nEndpoint: POST/GET /notify?title=...&body=...\n";
  s_server.send(200, "text/plain", msg);
}

static void handleNotify() {
  String title = s_server.arg("title");
  String body  = s_server.arg("body");
  if (body.length() == 0) body = s_server.arg("text");
  if (body.length() == 0) body = s_server.arg("msg");
  String timeArg = s_server.arg("time");
  String appArg  = s_server.arg("app");

  // Fallback jika dikirim sebagai raw JSON body
  if (title.length() == 0 && body.length() == 0 && s_server.hasArg("plain")) {
    String raw = s_server.arg("plain");
    raw.trim();
    if (raw.startsWith("{")) {
      title   = extractJsonField(raw, "title");
      body    = extractJsonField(raw, "body");
      if (body.length() == 0) body = extractJsonField(raw, "text");
      if (body.length() == 0) body = extractJsonField(raw, "msg");
      timeArg = extractJsonField(raw, "time");
      appArg  = extractJsonField(raw, "app");
    }
  }

  if (title.length() == 0 && body.length() == 0) {
    s_server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing title/body\"}");
    return;
  }
  if (title.length() == 0) title = "Notifikasi";

  if (appArg.length() > 0) {
    title = "[" + appArg + "] " + title;
  }

  char timeBuf[16];
  if (timeArg.length() > 0) {
    snprintf(timeBuf, sizeof(timeBuf), "%s", timeArg.c_str());
  } else {
    formatNowWita(timeBuf, sizeof(timeBuf));
  }

  notificationPush(title.c_str(), body.c_str(), timeBuf, StudioTheme::TEAL);
  Serial.printf("[NOTIFY] %s: %s (%s)\n", title.c_str(), body.c_str(), timeBuf);
  s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handleClear() {
  notificationClearAll();
  s_server.send(200, "application/json", "{\"ok\":true}");
}

static void handleStatus() {
  char buf[256];
  mqttNotifyGetStatus(buf, sizeof(buf));
  s_server.send(200, "text/plain", buf);
}

void notifyServerLoop() {
  if (WiFi.status() != WL_CONNECTED) return;

  if (!s_started) {
    s_started = true;
    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/notify", HTTP_ANY, handleNotify);
    s_server.on("/clear", HTTP_ANY, handleClear);
    s_server.on("/status", HTTP_GET, handleStatus);
    s_server.begin();

    if (MDNS.begin("mydeskmate")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("[NOTIFY] mDNS: http://mydeskmate.local/notify");
    }
    Serial.printf("[NOTIFY] HTTP Server ready: http://%s/notify\n",
                  WiFi.localIP().toString().c_str());
  }

  s_server.handleClient();
}
