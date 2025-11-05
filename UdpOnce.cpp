#include "UdpOnce.h"

static bool send_packet(const IPAddress& dest, uint16_t port, const uint8_t* p, size_t n) {
  WiFiUDP udp;
  if (!udp.begin(0)) return false;
  if (udp.beginPacket(dest, port) != 1) return false;
  udp.write(p, n);
  return (udp.endPacket() == 1);
}

bool UDP_SEND_ONCE(const IPAddress& dest, uint16_t port, const char* payload) {
  if (!payload) return false;
  return send_packet(dest, port, (const uint8_t*)payload, strlen(payload));
}

bool UDP_SEND_JSON_AGGREGATED(const IPAddress& dest, uint16_t port, const String& json,
                              size_t mtu_payload)
{
  const size_t len = json.length();
  if (len == 0) return false;

  // 1) そのまま送れるサイズなら一発
  if (len <= mtu_payload) {
    return UDP_SEND_ONCE(dest, port, json.c_str());
  }

  // 2) チャンク分割 ("CHUNK <sid> <idx>/<tot> " + payload_substring)
  //    sidは起動時間ベースの簡易ID
  const uint16_t sid = (uint16_t)(millis() & 0xFFFF);
  const size_t total = (len + mtu_payload - 1) / mtu_payload;

  bool all_ok = true;
  for (size_t i = 0; i < total; ++i) {
    const size_t off = i * mtu_payload;
    const size_t n   = (off + mtu_payload <= len) ? mtu_payload : (len - off);

    // ヘッダ生成
    // 例: "CHUNK 52341 3/10 " + データ
    char head[48];
    const int hl = snprintf(head, sizeof(head), "CHUNK %u %u/%u ", (unsigned)sid, (unsigned)(i+1), (unsigned)total);
    if (hl <= 0) { all_ok = false; break; }

    // 送出バッファを一時連結（小さいのでstackでOK）
    // 48 + 1200 ≈ 1248B / pkt
    uint8_t buf[48 + 1500];
    memcpy(buf, head, hl);
    memcpy(buf + hl, json.c_str() + off, n);

    if (!send_packet(dest, port, buf, hl + n)) {
      all_ok = false;
      // 続行はせず打ち切り（必要ならリトライ実装可）
      break;
    }

    delay(5); // ネットワークバースト緩和
  }
  return all_ok;
}
