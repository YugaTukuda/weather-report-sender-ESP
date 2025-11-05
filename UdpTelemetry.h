// UdpTelemetry.h
#pragma once
#include <Arduino.h>
#include <ESP8266WiFi.h>

void UdpTelemetry_begin(const char* ssid,
                        const char* pass,
                        const IPAddress& destIp,
                        uint16_t destPort);

// 毎ループで呼ぶだけ。intervalMsごとにダミーデータを1回送信
void UdpTelemetry_tick(uint32_t intervalMs);
