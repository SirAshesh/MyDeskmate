#pragma once

// ============================================================
// KONFIGURASI WIFI & SERVER QUOTA ANTIGRAVITY
// ============================================================
// Salin file ini menjadi include/wifi_config.h lalu isi SSID & password WiFi kamu.
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

// URL server HTTP lokal di PC (folder server/quota_server.py)
#define QUOTA_SERVER_URL "http://192.168.1.100:8787/quota.txt"

// Interval polling otomatis (ms)
#define QUOTA_POLL_MS    60000

// WebSocket server untuk menu Lyrics (script/ws-server)
#define LYRICS_WS_HOST   "192.168.100.80"
#define LYRICS_WS_PORT   8765
