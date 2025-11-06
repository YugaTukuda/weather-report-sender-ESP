// CCBT_wifi_earlyACK.ino
// RST起床 → NTP同期 → STMとWio互換セッション(全件受信・END) → 即ACK返却 → Soracom Inventory /publish(UNIX秒, 分割送信/詳細ログ) → DeepSleep
// 依存: ConfigPortal.h / CredStore.h（既存プロジェクトのもの）
// MCU: ESP8266

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <time.h> // ★ NTP (時刻取得) のために追加

// ========= Soracom Inventory (/publish) =========
static const char* DEVICE_ID     = "d-bne1ulprdk8oh07f6nn2";
static const char* DEVICE_SECRET = "dByvgJd/kZKrM0z19OTcdA==";
static const char* PUBLISH_URL   = "https://api.soracom.io/v1/devices/d-bne1ulprdk8oh07f6nn2/publish";

// ========= NTP (時刻取得) 設定 ★ 追加 =========
static const char* NTP_SERVER_1 = "ntp.nict.jp";
static const char* NTP_SERVER_2 = "pool.ntp.org";
static const long  NTP_TZ_SEC   = 9 * 3600; // JST (UTC+9)
static const int   NTP_DAYLIGHT = 0;        // 夏時間なし
static const uint32_t NTP_TIMEOUT_MS = 15000; // NTP同期タイムアウト

// ========= Config Portal / Credential Store =========
#include "ConfigPortal.h"
#include "CredStore.h"
#define AP_SSID  "ESP8266_SETUP"
#define AP_PASS  "esp8266ap"

// ========= UART（STM直結 / Wio互換プロトコル）=========
#define STM_BAUD          19200
#define WAKE_TOKEN_BYTE   0xA5

// ========= パラメータ =========
static const uint16_t BATCH_SIZE        = 50;     // 1バッチ件数（20〜40推奨）
static const uint16_t MAX_TOTAL         = 1800;   // 一回で送る上限
static const uint32_t HTTP_TIMEOUT_MS   = 30000;  // HTTPSタイムアウト
static const uint8_t  HTTP_RETRY_MAX    = 3;      // HTTPリトライ回数
static const uint16_t BATCH_INTERVAL_MS = 400;    // バッチ間隔
static const uint16_t UART_LINE_MAX     = 256;    // 1行の上限（越えたら破棄）
static const uint16_t UART_RXBUF        = 1024;   // 受信FIFOサイズ
static const uint8_t  TOKEN_RETRY_MAX   = 3;      // トークン再送回数
static const uint32_t TOKEN_RETRY_GAPMS = 200;    // 再送間隔
static const uint32_t START_WAIT_MS     = 20000;  // START待ち
static const uint32_t TS_WAIT_MS        = 6000;   // TS=待ち
static const uint32_t HEX_WAIT_MS       = 6000;   // HEX待ち
static const uint32_t END_WAIT_MS       = 1500;   // END参考待ち

// ====== デバッグマクロ ======
#define TRACE_ON 1
#if TRACE_ON
  #define DBG(fmt, ...)  do{ Serial.printf("[%9lu ms][%5u heap] " fmt "\n", millis(), ESP.getFreeHeap(), ##__VA_ARGS__); }while(0)
#else
  #define DBG(fmt, ...)  do{}while(0)
#endif

static void dump_line(const String& s, const char* tag="LINE") {
  if (!TRACE_ON) return;
  Serial.printf("[DUMP][%s] len=%u: '", tag, (unsigned)s.length());
  for (size_t i=0;i<s.length();++i){
    char c = s[i];
    if (c=='\r') Serial.print("\\r");
    else if (c=='\n') Serial.print("\\n");
    else if (c>=0x20 && c<=0x7e) Serial.print(c);
    else { Serial.print("\\x"); if ((uint8_t)c<0x10) Serial.print('0'); Serial.print((uint8_t)c,HEX); }
  }
  Serial.println("'");
}

// ===== DeepSleep =====
static void go_deepsleep_forever() {
  DBG("DeepSleep forever");
  delay(50);
  ESP.deepSleep(0); // 無期限（RSTで復帰）
}

// ===== 軽量行読み（改行必須, CR捨て, 上限超で破棄, WDT対策）=====
static bool uart_read_line(HardwareSerial& ser, String& out, uint32_t timeout_ms) {
  out = "";
  uint32_t ddl = millis() + timeout_ms;
  while ((int32_t)(millis() - ddl) < 0) {
    while (ser.available()) {
      char c = (char)ser.read();
      if (c == '\r') continue;
      if (c == '\n') return true;         // 改行で確定
      out += c;
      if (out.length() > UART_LINE_MAX) { // 行暴走→破棄してやり直し
        DBG("uart_read_line overflow (> %u), drop", UART_LINE_MAX);
        out = "";
        break;
      }
    }
    // WDT回避
    delay(0);
    yield();
  }
  // 改行未到達＝不完全行→無効
  out = "";
  return false;
}

// ===== HEX→bytes =====
static inline int hex2nib(char c) {
  if ('0'<=c && c<='9') return c-'0';
  if ('a'<=c && c<='f') return c-'a'+10;
  if ('A'<=c && c<='F') return c-'A'+10;
  return -1;
}
static bool hex_to_bytes_28(const char* hex, uint8_t out[28]) {
  for (int i=0;i<28;i++) {
    int hi = hex2nib(hex[i*2]);
    int lo = hex2nib(hex[i*2+1]);
    if (hi<0 || lo<0) return false;
    out[i] = (uint8_t)((hi<<4)|lo);
  }
  return true;
}

// ===== LE 読取り =====
static inline int16_t  rd_s16(const uint8_t* p){ return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
static inline uint16_t rd_u16(const uint8_t* p){ return (uint16_t)(p[0] | (p[1] << 8)); }
static inline int32_t  rd_s32(const uint8_t* p){ return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }
static inline uint32_t rd_u32(const uint8_t* p){ return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }

static void json_append_from_raw28(String& buf, const uint8_t raw[28],
                                   uint32_t ts_hdr, uint32_t time_offset_unix)
{
  // ---- 正しいオフセット（上表）----
  const uint32_t ts   = rd_u32(&raw[0x00]);   // 使わない運用でもOK
  const int16_t  a    = rd_s16(&raw[0x04]);
  const int16_t  b    = rd_s16(&raw[0x06]);
  const int16_t  c    = rd_s16(&raw[0x08]);
  const int16_t  t    = rd_s16(&raw[0x0A]);
  const int32_t  p    = rd_s32(&raw[0x0C]);
  const int16_t  h    = rd_s16(&raw[0x10]);
  const uint16_t xraw = rd_u16(&raw[0x12]);
  const int32_t  xmv  = rd_s32(&raw[0x14]);   // mV相当（int/float運用どちらでも可）
  // raw[0x18..0x19] 予備2B（rsv）
  // raw[0x1A..0x1B] crc16（読み出し不要）

  // ---- スケール & 丸め（Wio側に揃える）----
  const float tempC   = roundf((t   / 100.0f)  * 100.0f)  / 100.0f;
  const float pressKP = roundf((p   / 1000.0f) * 1000.0f) / 1000.0f;  // 例: 101.179 kPa
  const float humidP  = roundf((h   / 100.0f)  * 100.0f)  / 100.0f;
  const float mx      = fabsf(roundf((a / 100.0f) * 100.0f) / 100.0f); // 参照仕様では fabs
  const float my      =       roundf((b / 100.0f) * 100.0f) / 100.0f;
  const float mz      =       roundf((c / 100.0f) * 100.0f) / 100.0f;
  const float adc_f   = roundf(((float)xmv)     * 100.0f)  / 100.0f;   // 2桁丸め

  const uint32_t final_ts = (time_offset_unix > 0) ? (ts_hdr + time_offset_unix) : ts_hdr;

  // ---- JSON（フィールド名・桁数も揃える）----
  buf += F("{\"time\":");    buf += String(final_ts);
  buf += F(",\"temp\":");    buf += String(tempC, 2);
  buf += F(",\"press\":");   buf += String(pressKP, 3);   // ★ 3桁
  buf += F(",\"humid\":");   buf += String(humidP, 2);
  buf += F(",\"adc_mV\":");  buf += String(adc_f, 2);     // ★ 2桁
  buf += F(",\"mag\":{");
    buf += F("\"mag_x\":");  buf += String(mx, 2);  buf += F(",");
    buf += F("\"mag_y\":");  buf += String(my, 2);  buf += F(",");
    buf += F("\"mag_z\":");  buf += String(mz, 2);
  buf += F("}}");
}




  // ★ NTPで計算したオフセットを加算し、絶対UNIX時刻に変換
 

// ===== /publish POST（詳細ログ＋リトライ＋Connection: close＋WDTケア） =====
static bool post_publish(const String& payload, int& code_out, String& body_out) {
  BearSSL::WiFiClientSecure tls;
  tls.setTimeout(HTTP_TIMEOUT_MS);
  tls.setInsecure();                 // 本番はCA設定推奨
  tls.setBufferSizes(4096, 1024);    // 受信/送信バッファ（必要に応じ調整）

  HTTPClient https;
  https.setTimeout(HTTP_TIMEOUT_MS);
  https.setReuse(false); // keep-alive無効
  DBG("HTTP begin (len=%u)", payload.length());
  if (!https.begin(tls, PUBLISH_URL)) {
    DBG("HTTP begin failed");
    code_out = -1; body_out = F("BEGIN_FAILED");
    return false;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("X-Device-Secret", DEVICE_SECRET);
  https.addHeader("Connection", "close");

  bool ok = false;
  for (uint8_t attempt = 1; attempt <= HTTP_RETRY_MAX; ++attempt) {
    yield();
    delay(0);
    DBG("HTTP POST try=%u", attempt);
    int code = https.POST(payload);
    String resp = https.getString();
    DBG("HTTP code=%d", code);
    if (resp.length() && TRACE_ON) Serial.println(resp); // DEMコード等をそのままダンプ
    code_out = code; body_out = resp;
    if (code > 0 && code >= 200 && code < 300) { ok = true; break; }
    if (attempt < HTTP_RETRY_MAX) {
      uint32_t backoff = 200U * attempt;
      DBG("HTTP retry in %u ms", backoff);
      delay(backoff);
      yield();
    }
  }

  https.end();
  if (!ok && code_out == 0) { code_out = -2; if (body_out.isEmpty()) body_out = F("NO_RESPONSE"); }
  return ok;
}

// ======== STMセッション(全件受信) → 即ACK返却 → /publish分割送信(詳細ログ) ========
// ★ NTPで取得した現在時刻 (UNIX秒) を引数で受け取る
static bool run_and_upload_session(uint32_t ntp_now_unix) {
  // UART開始（STM直結）
  Serial.flush();
  Serial.begin(STM_BAUD);
  Serial.setRxBufferSize(UART_RXBUF);
  DBG("UART started, RXBUF=%u", UART_RXBUF);
  delay(5);

  // 受信残骸掃除
  uint32_t ddl = millis() + 3;
  while ((int32_t)(millis()-ddl) < 0) { while (Serial.available()) (void)Serial.read(); delay(0); }

  // // トークン送信（最大 TOKEN_RETRY_MAX 回）
  // for (uint8_t k=0;k<TOKEN_RETRY_MAX;++k) {
  //   Serial.write((uint8_t)WAKE_TOKEN_BYTE);
  //   Serial.flush();
  //   DBG("Sent WAKE TOKEN (0x%02X) try=%u", WAKE_TOKEN_BYTE, k+1);
  //   delay(TOKEN_RETRY_GAPMS);
  //   yield();
  // }
  delay(200);
  Serial.write((uint8_t)WAKE_TOKEN_BYTE);
  Serial.flush();
  DBG("Sent WAKE TOKEN (0x%02X) try=%u", WAKE_TOKEN_BYTE, 1);

  // START <count> を待つ（ゴミ行は全て捨てる）
  String line;
  uint32_t tstart_deadline = millis() + START_WAIT_MS;
  long count_hdr = -1;
  while ((int32_t)(millis()-tstart_deadline) < 0) {
    if (!uart_read_line(Serial, line, 1000)) { delay(0); yield(); continue; }
    if (line.length()==0) { delay(0); yield(); continue; }
    if (line.startsWith("START ")) {
      if (sscanf(line.c_str(), "START %ld", &count_hdr) == 1 && count_hdr > 0) break;
      DBG("START line parse fail"); dump_line(line,"START?");
      count_hdr = -1; // 継続
    } else {
      dump_line(line, "SKIP");
    }
    delay(0); yield();
  }
  if (count_hdr <= 0) { DBG("START timeout or invalid"); return false; }

  uint16_t target = (uint16_t)count_hdr;
  if (target > MAX_TOTAL) target = MAX_TOTAL;
  DBG("START count=%u", target);

  // メモリ確保（全件分）
  struct RawSlot { uint8_t raw[28]; uint32_t ts_hdr; };
  size_t alloc_sz = sizeof(RawSlot) * target;
  RawSlot* slots = (RawSlot*)malloc(alloc_sz);
  if (!slots) { DBG("malloc(%u) failed", (unsigned)alloc_sz); return false; }
  DBG("malloc ok (%u bytes)", (unsigned)alloc_sz);

  // 受信ループ
  uint16_t frames_ok = 0;
  uint32_t max_ts_good = 0;

  for (uint16_t i=0; i<target; ++i) {
    // TS=... を探す
    bool got_ts = false;
    for (uint8_t scan=0; scan<8; ++scan) {
      if (!uart_read_line(Serial, line, TS_WAIT_MS)) { DBG("TS timeout at i=%u", i); break; }
      if (line.startsWith("TS=")) { got_ts = true; break; }
      dump_line(line, "DROP");
      delay(0); yield();
    }
    if (!got_ts) { DBG("TS resync fail at i=%u", i); break; }

    uint32_t ts_hdr = 0;
    if (sscanf(line.c_str()+3, "%lu", &ts_hdr) != 1) {
      DBG("TS value parse fail at i=%u", i); dump_line(line,"TS?"); break;
    }

    // HEX（56文字）
    if (!uart_read_line(Serial, line, HEX_WAIT_MS)) { DBG("HEX timeout at i=%u", i); break; }
    if (line.length() != 56) { DBG("HEX len=%u (expected 56) at i=%u", (unsigned)line.length(), i); dump_line(line,"HEX?"); break; }
    if (!hex_to_bytes_28(line.c_str(), slots[frames_ok].raw)) { DBG("HEX parse fail at i=%u", i); dump_line(line,"HEX!"); break; }

    slots[frames_ok].ts_hdr = ts_hdr;
    if (ts_hdr > max_ts_good) max_ts_good = ts_hdr;
    frames_ok++;

    // ★ (デバッグログ追記 1/2) STMから受信したts_hdrをそのまま表示
    if (TRACE_ON) {
      Serial.printf("[RECV] i=%u ts_hdr=%lu\n", (unsigned)i, (unsigned long)ts_hdr);
    }


    if ((frames_ok % 50) == 0) { DBG("recv %u/%u ...", frames_ok, target); }
    if ((frames_ok % 10) == 0) { delay(0); yield(); }
  }

  // END（参考）
  (void)uart_read_line(Serial, line, END_WAIT_MS);
  if (line.length()) dump_line(line,"TAIL");

  DBG("RECV DONE frames_ok=%u, max_ts_good=%lu", frames_ok, (unsigned long)max_ts_good);

  // ★★★ デバッグログ追記: 受信した全ts_hdrを一覧表示 ★★★
  if (TRACE_ON && frames_ok > 0) {
    DBG("--- START Dump All Received ts_hdr (frames_ok=%u) ---", frames_ok);
    String ts_line = "";
    ts_line.reserve(128); // 1行あたりのバッファ
    for (uint16_t i = 0; i < frames_ok; ++i) {
      if (i > 0) ts_line += ", ";
      
      // 1行が長くなりすぎたら改行 (approx 10 per line)
      if (ts_line.length() > 100) {
        Serial.println(ts_line);
        ts_line = "";
      }
      ts_line += String(slots[i].ts_hdr);
    }
    // 最後の行を出力
    if (ts_line.length() > 0) {
      Serial.println(ts_line);
    }
    DBG("--- END Dump All Received ts_hdr ---");
  }
  // ★★★ 追記ここまで ★★★


  // ========= ★ NTPオフセット計算 ★ =========
  // (NTP時刻 - STMの最新相対秒数) = オフセット
  uint32_t time_offset_unix = 0;
  if (frames_ok > 0 && ntp_now_unix > max_ts_good) {
    time_offset_unix = ntp_now_unix - max_ts_good;
    DBG("NTP offset calculated: now=%lu - max_ts=%lu = %lu", (unsigned long)ntp_now_unix, (unsigned long)max_ts_good, (unsigned long)time_offset_unix);
  } else {
    DBG("NTP offset SKIPPED (ntp=%lu, max_ts=%lu). Using relative time.", (unsigned long)ntp_now_unix, (unsigned long)max_ts_good);
  }
  // (NTP失敗時(ntp_now_unix=0) や max_ts_good がおかしい場合は offset=0 となり、
  // json_append... 側でフォールバック処理される)

  delay(100);

  // ========= ここで即ACK（要求通り：受信直後に返す）=========
  {
    char ack[32];
    unsigned long ack_ts = (frames_ok > 0) ? max_ts_good : 0UL; // 受信できていれば最大TS、失敗なら0
    // snprintf(ack, sizeof(ack), "ACK %lu\r\n", ack_ts);
    Serial.write((uint8_t)0xBB);
    Serial.print(ack); // ★注意: ack[32] は初期化されていないため、不定な文字列が送信されます
    Serial.flush();
    DBG("ACK sent EARLY: %s", ack); // ★注意: ack は不定値です
  }

  if (frames_ok == 0) {
    free(slots);
    return false; // 以降のHTTPは行わない
  }

  // ========= ここからHTTP分割送信（詳細ログ/201,207,400等を表示）=========
  uint32_t posted = 0, batches = 0;
  bool all_ok = true;

  int first_code = 0;          // 最初のHTTPコード
  int last_code  = 0;          // 最後のHTTPコード
  int worst_code = 0;          // 200台以外を記録（代表エラーコード）
  uint32_t code_2xx = 0, code_3xx = 0, code_4xx = 0, code_5xx = 0, code_err = 0;

  String last_body;

  String batchBody; 
  batchBody.reserve((size_t)BATCH_SIZE * 220);

  auto classify_code = [&](int code){
    if      (code >= 200 && code < 300) code_2xx++;
    else if (code >= 300 && code < 400) code_3xx++;
    else if (code >= 400 && code < 500) { code_4xx++; worst_code = (worst_code==0?code:worst_code); }
    else if (code >= 500 && code < 600) { code_5xx++; worst_code = (worst_code==0?code:worst_code); }
    else                                code_err++;
  };

  auto flush_batch = [&](uint16_t rec_in_batch, bool final)->bool {
    if (rec_in_batch == 0) return true;
    String payload; payload.reserve(batchBody.length()+16);
    payload += F("{\"data\":[");
    payload += batchBody;
    payload += F("]}");
    if (TRACE_ON) Serial.println(payload); // デバッグ用にペイロード表示

    DBG("POST batch rec=%u len=%u", (unsigned)rec_in_batch, (unsigned)payload.length());
    int code = 0; String body;
    bool ok = post_publish(payload, code, body);

    if (first_code == 0) first_code = code;
    last_code = code;
    last_body = body;
    classify_code(code);

    Serial.printf("[HTTP][BATCH #%lu] code=%d (%s)\n",
                  (unsigned long)(batches+1),
                  code,
                  (ok ? "SUCCESS" : "FAIL"));

    if (!ok) {
      all_ok = false; // 失敗しても続ける（できるだけ他バッチも投げ切る）
    }

    batchBody.remove(0); // capacity保持
    delay(BATCH_INTERVAL_MS);
    yield();
    return ok;
  };

  uint16_t inBatch = 0;
  for (uint16_t i=0; i<frames_ok; ++i) {
    if (inBatch > 0) batchBody += ",";
    // ★ オフセットを渡す
    json_append_from_raw28(batchBody, slots[i].raw, slots[i].ts_hdr, time_offset_unix); 
    inBatch++;

    if (inBatch >= BATCH_SIZE) {
      (void)flush_batch(inBatch, false);
      posted += inBatch;
      batches++;
      inBatch = 0;
      DBG("posted=%lu / frames_ok=%u (batches=%lu)", (unsigned long)posted, frames_ok, (unsigned long)batches);
    }
    if ((i % 10) == 0) { delay(0); yield(); }
  }
  if (inBatch > 0) {
    (void)flush_batch(inBatch, true);
    posted += inBatch;
    batches++;
  }

  DBG("HTTP SUMMARY: first=%d last=%d worst=%d  2xx=%lu 3xx=%lu 4xx=%lu 5xx=%lu err=%lu",
      first_code, last_code, worst_code,
      (unsigned long)code_2xx,(unsigned long)code_3xx,(unsigned long)code_4xx,(unsigned long)code_5xx,(unsigned long)code_err);

  // 人間が見やすいRESULT行（STM側で任意に読む用）
  {
    String msg = last_body;
    msg.replace("\r"," "); msg.replace("\n"," ");
    if (msg.length() > 80) msg = msg.substring(0,80) + "...";
    int rep = (worst_code != 0) ? worst_code : last_code;
    Serial.printf("RESULT %d batches=%lu posted=%lu/%u 2xx=%lu 3xx=%lu 4xx=%lu 5xx=%lu err=%lu %s\r\n",
                  rep, (unsigned long)batches, (unsigned long)posted, frames_ok,
                  (unsigned long)code_2xx,(unsigned long)code_3xx,(unsigned long)code_4xx,(unsigned long)code_5xx,(unsigned long)code_err,
                  msg.c_str());
    Serial.flush();
  }

  free(slots);
  DBG("SESSION DONE frames_ok=%u posted=%lu batches=%lu all_ok=%d", frames_ok, (unsigned long)posted, (unsigned long)batches, (int)all_ok);
  return all_ok;
}

// ========== フロー管理 ==========
enum class AppState { BOOT, CONFIG_AP, WIFI_SEND, SLEEP };
static AppState g_state = AppState::BOOT;
static String g_ssid, g_pass;

void setup() {
  Serial.begin(STM_BAUD);
  Serial.setRxBufferSize(UART_RXBUF);
  delay(20);

  Serial.println();
  Serial.printf("[BOOT] resetReason=%s\n", ESP.getResetReason().c_str());
  Serial.println(F("[BOOT] Wio-compatible session on ESP8266 -> Inventory /publish (UNIX秒)"));

  WiFi.setSleep(false); // 送信中の遅延対策（必要に応じてtrueに）
  CredStore_begin();

  if (CredStore_load(g_ssid, g_pass)) {
    Serial.print(F("[CRED] SSID=")); Serial.print(g_ssid);
    Serial.print(F(" PASS=")); Serial.println(g_pass.length()?F("********"):F("(empty)"));
    g_state = AppState::WIFI_SEND;
  } else {
    Serial.println(F("[CRED] not found → Config AP"));
    ConfigPortal_begin(AP_SSID, AP_PASS);
    g_state = AppState::CONFIG_AP;
  }
}

void loop() {
  switch (g_state) {
    case AppState::CONFIG_AP: {
      ConfigPortal_tick();
      if (ConfigPortal_hasCredentials()) {
        ConfigPortal_getCredentials(g_ssid, g_pass);
        Serial.print(F("[CFG] SSID=")); Serial.print(g_ssid);
        Serial.print(F(" PASS=")); Serial.println(g_pass.length()?F("********"):F("(empty)"));
        if (CredStore_save(g_ssid, g_pass)) Serial.println(F("[CRED] saved"));
        else                                 Serial.println(F("[CRED] save failed"));
        ConfigPortal_end();
        g_state = AppState::SLEEP; // 次回RSTから運用
      }
      yield();
      break;
    }

    case AppState::WIFI_SEND: {
      WiFi.mode(WIFI_STA);
      Serial.print(F("[WiFi] connect ")); Serial.println(g_ssid);
      WiFi.begin(g_ssid.c_str(), g_pass.c_str());

      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(200);
        Serial.print(".");
        yield();
      }
      Serial.println();

      if (WiFi.status() == WL_CONNECTED) {
        Serial.println(F("[NET] WiFi connected."));
        
        // ========= ★ NTP (時刻同期) 処理 ★ =========
        DBG("NTP sync starting... (TZ=JST-9)");
        configTime(NTP_TZ_SEC, NTP_DAYLIGHT, NTP_SERVER_1, NTP_SERVER_2);
        
        time_t now = time(nullptr);
        uint32_t ntp_start_ms = millis();
        while (now < 1672531200UL && (millis() - ntp_start_ms) < NTP_TIMEOUT_MS) { // 2023年より前なら未同期とみなす
          delay(100);
          yield();
          now = time(nullptr);
        }

        uint32_t ntp_now_unix = 0;
        if (now >= 1672531200UL) {
          ntp_now_unix = (uint32_t)now;
          DBG("NTP sync OK. UnixTime = %lu", (unsigned long)ntp_now_unix);
        } else {
          DBG("NTP sync FAILED (timeout). Using relative time.");
        }
        // ===========================================

        Serial.println(F("[NET] Starting STM session & /publish upload (EARLY ACK MODE)"));
        bool ok = run_and_upload_session(ntp_now_unix); // ★ NTP時刻を渡す
        Serial.print(F("[RESULT] ")); Serial.println(ok ? F("OK") : F("FAIL"));

      } else {
        Serial.println(F("[WiFi] connect FAIL (skip)"));
      }
      g_state = AppState::SLEEP;
      break;
    }

    case AppState::SLEEP: {
      go_deepsleep_forever();
      break;
    }

    case AppState::BOOT:
    default: break;
  }
}
