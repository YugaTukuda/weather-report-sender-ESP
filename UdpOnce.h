#pragma once
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

// 生文字列を1回UDP送信
bool UDP_SEND_ONCE(const IPAddress& dest, uint16_t port, const char* payload);

// JSONをまとめて送信（サイズに応じて単発 or 分割チャンク送信）
bool UDP_SEND_JSON_AGGREGATED(const IPAddress& dest, uint16_t port, const String& json,
                              size_t mtu_payload = 1200); // 1200B/chunk目安
