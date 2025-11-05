#include "STMProtocol.h"

// ---- 受信レコード定義 -------------------------------------------------
#pragma pack(push,1)
typedef struct {
  uint32_t ts;
  int16_t  a;   // mag_x * 100
  int16_t  b;   // mag_y * 100
  int16_t  c;   // mag_z * 100
  int16_t  t;   // 温度(℃*100)
  int32_t  p;   // 気圧(hPa*1000)
  int16_t  h;   // 湿度(%*100)
  uint16_t xraw;// ADC raw
  uint16_t xmv; // ADC mV
  uint32_t rsv; // 予約
  uint16_t crc16;
} rec28_t;
#pragma pack(pop)

static uint16_t crc16_modbus(const uint8_t* p, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i=0;i<n;i++){
    crc ^= p[i];
    for(int b=0;b<8;b++){
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else         crc = (crc >> 1);
    }
  }
  return crc;
}

static uint16_t crc16_ccitt(const uint8_t* p, size_t n) {
  uint16_t crc = 0xFFFF; // 0x1021, init 0xFFFF（必要なら0x1D0F等に変更）
  for (size_t i=0;i<n;i++){
    crc ^= (uint16_t)p[i] << 8;
    for (int b=0;b<8;b++){
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else              crc = (crc << 1);
    }
  }
  return crc;
}

bool readLineWithDeadline(Stream& s, String& out, uint32_t deadline_ms) {
  out = "";
  while (millis() < deadline_ms) {
    while (s.available()) {
      char c = (char)s.read();
      if (c == '\r') continue;
      if (c == '\n') return true;
      out += c;
    }
    delay(1);
  }
  return false;
}

static bool parse_hex_56(const String& line, uint8_t bytes[28]) {
  if (line.length() != 56) return false;
  for (int i=0;i<28;i++){
    char hi = line[2*i], lo = line[2*i+1];
    auto hexv = [](char ch)->int {
      if ('0'<=ch && ch<='9') return ch-'0';
      if ('a'<=ch && ch<='f') return 10+ch-'a';
      if ('A'<=ch && ch<='F') return 10+ch-'A';
      return -1;
    };
    int h = hexv(hi), l = hexv(lo);
    if (h<0 || l<0) return false;
    bytes[i] = (uint8_t)((h<<4) | l);
  }
  return true;
}

static bool verify_crc(const rec28_t& r, CRC16Mode mode) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&r);
  const size_t n = sizeof(rec28_t) - sizeof(uint16_t);
  uint16_t calc = (mode==CRC16Mode::MODBUS_IBM) ? crc16_modbus(p, n)
                                                : crc16_ccitt(p, n);
  return (calc == r.crc16);
}

static void rec_to_json(const rec28_t& r, String& out) {
  // 変換スケールに注意
  float temp  = r.t / 100.0f;
  float press = r.p / 1000.0f;
  float humid = r.h / 100.0f;
  float mv    = r.xmv; // mVは整数→そのまま
  float mx = r.a / 100.0f, my = r.b / 100.0f, mz = r.c / 100.0f;

  // 単一オブジェクトのJSONを生成
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"time\":%lu,\"temp\":%.2f,\"press\":%.3f,\"humid\":%.2f,"
    "\"adc_mV\":%.2f,\"mag\":{\"mag_x\":%.2f,\"mag_y\":%.2f,\"mag_z\":%.2f}}",
    (unsigned long)r.ts, temp, press, humid, mv, mx, my, mz);
  out = buf;
}

bool STM_run_session(Stream& io, STM_SessionResult& out,
                     CRC16Mode mode, uint32_t line_timeout_ms)
{
  out = STM_SessionResult{};
  // 1) 起床トークン
  uint8_t tok = 0xA5;
  io.write(&tok, 1);
  io.flush();

  // 2) START <count>
  String line;
  if (!readLineWithDeadline(io, line, millis()+line_timeout_ms)) return false;
  if (!line.startsWith("START ")) return false;
  long count = line.substring(6).toInt();

  // 3) ループで TS + hex56 を受信
  String jsonArray = "{\"data\":[";
  bool first = true;
  uint32_t max_ts_ok = 0;
  uint32_t ok=0, bad=0;

  for (long i=0;i<count;i++) {
    // TS=<sec>
    if (!readLineWithDeadline(io, line, millis()+line_timeout_ms)) { bad++; continue; }
    if (!line.startsWith("TS="))                                    { bad++; continue; }
    uint32_t ts = (uint32_t) line.substring(3).toInt();

    // 56 hex
    if (!readLineWithDeadline(io, line, millis()+line_timeout_ms)) { bad++; continue; }
    uint8_t raw[28];
    if (!parse_hex_56(line, raw)) { bad++; continue; }

    // エンディアン：STM側がLEで投げている前提（Wio実装準拠）
    rec28_t rec;
    memcpy(&rec, raw, sizeof(rec));

    // CRCチェック
    if (!verify_crc(rec, mode)) { bad++; continue; }

    // TS整合（構造体のtsと行のTSが一致するのが自然）
    if (rec.ts != ts) { bad++; continue; }

    // JSON化
    String one;
    rec_to_json(rec, one);
    if (!first) jsonArray += ",";
    jsonArray += one;
    first = false;

    if (rec.ts > max_ts_ok) max_ts_ok = rec.ts;
    ok++;
  }

  // 4) END
  if (!readLineWithDeadline(io, line, millis()+line_timeout_ms)) return false;
  if (line != "END") return false;

  // 5) ACK <max_ts_good>
  {
    char ack[32];
    snprintf(ack, sizeof(ack), "ACK %lu\n", (unsigned long)max_ts_ok);
    io.print(ack);
    io.flush();
  }

  jsonArray += "]}";
  out.jsonPayload = jsonArray;
  out.maxTsGood   = max_ts_ok;
  out.okCount     = ok;
  out.badCount    = bad;
  return true;
}
