// UdpTelemetry.cpp
#include "UdpTelemetry.h"
#include <WiFiUdp.h>

namespace {
  WiFiUDP udp;
  IPAddress g_destIp;
  uint16_t  g_destPort = 0;
  uint32_t  g_lastSend = 0;
  uint32_t  g_seq      = 0;

  void connectWiFi_(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    Serial.print("[WiFi] Connecting to "); Serial.println(ssid);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("[WiFi] Connected. IP = "); Serial.println(WiFi.localIP());
    } else {
      Serial.println("[WiFi] Failed to connect. Rebooting in 5s...");
      delay(5000);
      ESP.restart();
    }
  }

  // ここを“1つの関数”にまとめたコア送信処理
  void sendDummyOnce_() {
    g_seq++;

    // ダミー計測値の生成
    float temp = 20.0f + (g_seq % 10);  // 20〜29℃
    float humi = 50.0f + (g_seq % 5);   // 50〜54%
    float vbat = 3.70f + 0.01f * (g_seq % 6);

    // JSON風テキスト
    char buf[256];
    snprintf(buf, sizeof(buf),
      "{\"type\":\"esp8266_demo\",\"seq\":%lu,\"ms\":%lu,"
      "\"rssi\":%d,\"ip\":\"%s\",\"temp\":%.2f,\"humi\":%.2f,\"vbat\":%.2f}",
      (unsigned long)g_seq, (unsigned long)millis(),
      WiFi.RSSI(), WiFi.localIP().toString().c_str(),
      temp, humi, vbat);

    udp.beginPacket(g_destIp, g_destPort);
    udp.write(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
    udp.endPacket();

    Serial.print("[SEND] "); Serial.println(buf);
  }
} // namespace

void UdpTelemetry_begin(const char* ssid,
                        const char* pass,
                        const IPAddress& destIp,
                        uint16_t destPort)
{
  connectWiFi_(ssid, pass);
  if (!udp.begin(0)) {
    Serial.println("[UDP] begin() failed");
  } else {
    Serial.print("[UDP] local port = "); Serial.println(udp.localPort());
  }
  g_destIp   = destIp;
  g_destPort = destPort;
  g_lastSend = 0;
  g_seq      = 0;
}

void UdpTelemetry_tick(uint32_t intervalMs) {
  if (millis() - g_lastSend >= intervalMs) {
    g_lastSend = millis();
    sendDummyOnce_();  // ← “ひとつの関数”に集約した送信処理
  }
}
