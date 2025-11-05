/*
  Wio BG770A  <--UART-->  STM32 (FRAM保持)
  プロトコル:
    [Wio] (D30↑割り込みで起床)
    [Wio] 0xA5          // ★単一バイトのトークン
    [STM] "START <count>"
    [STM] "TS=<sec>" + "\n<payload-56hex>"  // 28B→56hex
        ... 繰り返し ...
    [STM] "END"
    [Wio] "ACK <max_ts_good>"   // CRC/TSがOKだった最大全TS

  配線:
    D30: STM からの起床パルス入力 (立ち上がり)
    Serial1: STM と 19200 8N1
    Serial : PC へログ（任意）
*/

#include <Adafruit_TinyUSB.h>
#include <WioCellular.h>
#include <ArduinoJson.h>

/* =============================
 *  ネットワーク有効/無効 切替
 *  1: ネットワーク無効（検証のみ）
 *  0: ネットワーク有効（実送信）
 * ============================= */
#define DISABLE_NETWORK 0 //1でサーバー通信OFF, 0でサーバー通信ON

// ---- ネットワーク設定（残すが、DISABLE_NETWORK=1なら実行しない） ----
#define SEARCH_ACCESS_TECHNOLOGY (WioCellularNetwork::SearchAccessTechnology::LTEM)
#define LTEM_BAND (WioCellularNetwork::NTTDOCOMO_LTEM_BAND)
static const char* APN = "soracom.io";
static const unsigned long POWER_ON_TIMEOUT = 60000;
static const unsigned long NETWORK_TIMEOUT  = 60000;

// ---- 送信先（残す） ----
static const char* HOST = "harvest.soracom.io";
static const int   PORT = 8514;
static const unsigned long CONNECT_TIMEOUT = 10000;
static const unsigned long RECEIVE_TIMEOUT = 10000;

// ---- UART/タイミング設定 ----
static const int PIN_TRIG      = 30;     // D30: 起床入力
static const uint32_t UART_BAUD= 19200;
static const uint8_t WAKE_TOKEN= 0xA5;   // ★READYではなく0xA5
static const uint8_t HS_TOKEN   = 0xBB;   // 初期化時のハンドシェイク0xBB
static const unsigned long START_TIMEOUT_MS   = 15000;
static const unsigned long TS_TIMEOUT_MS      = 5000;
static const unsigned long PAYLOAD_TIMEOUT_MS = 3000;

// ---- グローバル ----
volatile bool g_triggered = false;
static JsonDocument JsonDoc;

// ============ Uno版 28B レコード定義（必須） ============
#pragma pack(push,1)
typedef struct {
  uint32_t ts;     // 秒
  int16_t  a;      // 例: 磁気X (uT*100)
  int16_t  b;      // 例: 磁気Y (uT*100)
  int16_t  c;      // 例: 磁気Z (uT*100)
  int16_t  t;      // 温度 C*100
  int32_t  p;      // 気圧 hPa*1000
  int16_t  h;      // 湿度 %*100
  uint16_t xraw;   // ADC raw
  uint16_t xmv;    // ADC mV
  uint32_t rsv;    // 予約
  uint16_t crc16;  // 先頭26BのCRC-CCITT(0x1021, init=0xFFFF)
} rec28_t;
#pragma pack(pop)
static_assert(sizeof(rec28_t)==28, "rec28 must be 28 bytes");

// ============ ユーティリティ ============
static inline void onTrigRise(){ g_triggered = true; }

static uint16_t crc16_ccitt(const uint8_t* p, size_t len){
  uint16_t crc = 0xFFFF;
  for (size_t i=0;i<len;i++){
    crc ^= (uint16_t)p[i] << 8;
    for (int b=0;b<8;b++){
      crc = (crc & 0x8000) ? (uint16_t)((crc<<1)^0x1021) : (uint16_t)(crc<<1);
    }
  }
  return crc;
}

static inline int hexNibble(char c){
  if (c>='0'&&c<='9') return c-'0';
  if (c>='A'&&c<='F') return c-'A'+10;
  if (c>='a'&&c<='f') return c-'a'+10;
  return -1;
}

static bool hex56_to_bytes28(const String& hex, uint8_t out[28]){
  if (hex.length()!=56) return false;
  for (int i=0;i<28;i++){
    int hi=hexNibble(hex[i*2]), lo=hexNibble(hex[i*2+1]);
    if (hi<0||lo<0) return false;
    out[i]=(uint8_t)((hi<<4)|lo);
  }
  return true;
}

// \rを捨て \n で終端、締切つき
static bool readLineWithDeadline(HardwareSerial &ser, String &out, unsigned long deadline_ms){
  out = "";
  while ((long)(millis()-deadline_ms)<0){
    while (ser.available()){
      int ch=ser.read();
      if (ch<0) break;
      if (ch=='\r') continue;
      if (ch=='\n') return true;
      out+=(char)ch;
    }
    delay(1);
  }
  return false;
}

// “TS=”が来るまで同期（ズレ復帰）
static bool syncToTS(HardwareSerial &ser, unsigned long timeout_ms){
  const char* pat="TS="; uint8_t m=0;
  unsigned long ddl=millis()+timeout_ms;
  while ((long)(millis()-ddl)<0){
    while (ser.available()){
      char c=(char)ser.read();
      if (c==pat[m]){ m++; if (m==3) return true; }
      else { m = (c=='T') ? 1 : 0; }
    }
  }
  return false;
}

// "TS=<dec>" を \n までパース
static bool readTSnumber(HardwareSerial &ser, unsigned long &ts_out, unsigned long timeout_ms){
  char buf[16]; int n=0;
  unsigned long ddl=millis()+timeout_ms;
  while ((long)(millis()-ddl)<0){
    while (ser.available()){
      char c=(char)ser.read();
      if (c=='\r') continue;
      if (c=='\n'){
        buf[n]=0;
        ts_out = strtoul(buf,nullptr,10);
        return (n>0);
      }
      if (c>='0'&&c<='9'){ if (n<15) buf[n++]=c; }
      else return false; // 変な文字→ズレ
    }
  }
  return false;
}

// 16進文字だけを56個集める（CR/LF/ゴミは捨てる）
static bool readHex56(HardwareSerial &ser, char out56[57], unsigned long timeout_ms){
  int k=0; unsigned long ddl=millis()+timeout_ms;
  while ((long)(millis()-ddl)<0){
    while (ser.available()){
      char c=(char)ser.read();
      bool isHex=(c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f');
      if (isHex){
        out56[k++]=c;
        if (k==56){ out56[56]='\0'; return true; }
      }
      // その他は捨てる
    }
  }
  return false;
}

static void flushInputQuick(HardwareSerial &ser){
  unsigned long until=millis()+5;
  while ((long)(millis()-until)<0 && ser.available()) (void)ser.read();
}

// Soracom送信（DISABLE_NETWORKで切替）
template<typename T>
void printData(T &stream, const void *data, size_t size) {
  auto p = static_cast<const char *>(data);
  for (; size > 0; --size, ++p)
    stream.write(0x20 <= *p && *p <= 0x7f ? *p : '.');
}

static bool sendJson(const JsonDocument &doc) {
#if DISABLE_NETWORK
  // ★オフライン：送信内容を表示のみ
  String str; serializeJson(doc, str);
  Serial.println("[OFFLINE] Would send JSON:");
  Serial.println(str);
  return true;  // 表示できたらOK扱い
#else
  Serial.println("### Sending");
  Serial.print("Connecting "); Serial.print(HOST); Serial.print(":"); Serial.println(PORT);
  WioCellularTcpClient client(WioCellular, 1);
  if (!client.connect(HOST, PORT)) { Serial.println("ERROR: connect"); return false; }

  String str; serializeJson(doc, str);
  Serial.print("Sending "); printData(Serial, str.c_str(), str.length()); Serial.println();
  if (client.print(str)==0){ Serial.println("ERROR: send"); client.stop(); return false; }

  Serial.println("Receiving");
  unsigned long timeout=millis()+RECEIVE_TIMEOUT;
  while (client.available()==0){
    if (millis()>timeout){ Serial.println("ERROR: recv timeout"); client.stop(); return false; }
    delay(1);
  }
  while (client.available()) Serial.write(client.read());
  Serial.println();
  client.stop(); Serial.println("### Completed");
  return true;
#endif
}

// ネットワーク初期化（DISABLE_NETWORKで切替）
static void setupNetwork(){
#if DISABLE_NETWORK
  Serial.println("[Wio] Network init skipped (DISABLE_NETWORK=1)");
#else
  Serial.println("[Wio] Network init...");
  pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, HIGH);

  WioNetwork.config.searchAccessTechnology = SEARCH_ACCESS_TECHNOLOGY;
  WioNetwork.config.ltemBand = LTEM_BAND;
  WioNetwork.config.apn = APN;

  WioCellular.begin();
  WioNetwork.abortHandler = [](const char* f,int l){ Serial.print("ERROR "); Serial.print(f); Serial.print(":"); Serial.println(l); Serial.flush(); abort(); };
  Serial.println("[Wio] Powering on...");
  if (WioCellular.powerOn(POWER_ON_TIMEOUT) != WioCellularResult::Ok){ Serial.println("powerOn FAIL"); abort(); }
  WioNetwork.begin();
  Serial.println("[Wio] Wait network...");
  if (!WioNetwork.waitUntilCommunicationAvailable(NETWORK_TIMEOUT)){ Serial.println("network timeout"); abort(); }
  Serial.println("[Wio] Network OK");
  digitalWrite(LED_BUILTIN, LOW);
#endif
}

// ============ センサ28Bの検証＆送信 ============
static void handle_good_frame_and_maybe_send(const rec28_t& r){
  // まずは検証内容をシリアル出力（常時）
  Serial.print("[Wio] OK frame: ts="); Serial.print(r.ts);
  Serial.print(" a=");  Serial.print(r.a);
  Serial.print(" b=");  Serial.print(r.b);
  Serial.print(" c=");  Serial.print(r.c);
  Serial.print(" t=");  Serial.print(r.t);
  Serial.print(" p=");  Serial.print(r.p);
  Serial.print(" h=");  Serial.print(r.h);
  Serial.print(" xraw="); Serial.print(r.xraw);
  Serial.print(" xmv=");  Serial.println(r.xmv);

  // JSON作成（ネットワークOFFでも内容確認できる）
  JsonDoc.clear();
  JsonDoc["timestamp"] = r.ts;
  JsonDoc["device_id"] = "wio_bg770a";
  JsonObject mag = JsonDoc["mag"].to<JsonObject>();
  mag["x_uT"] = r.a / 100.0f;
  mag["y_uT"] = r.b / 100.0f;
  mag["z_uT"] = r.c / 100.0f;
  JsonObject env = JsonDoc["env"].to<JsonObject>();
  env["temp_C"]      = r.t / 100.0f;
  env["pressure_hPa"]= r.p / 1000.0f;
  env["humidity_%"]  = r.h / 100.0f;
  JsonObject adc = JsonDoc["adc"].to<JsonObject>();
  adc["raw"] = r.xraw;
  adc["mV"]  = r.xmv;
  JsonDoc["crc_status"] = "OK";
  JsonDoc["ts_status"]  = "OK";

  (void)sendJson(JsonDoc);  // DISABLE_NETWORKに応じて送信/表示のみ
}

// ============ Arduino 標準関数 ============
void setup(){
  Serial.begin(115200);
  unsigned long t0=millis(); while (!Serial && millis()-t0<2000) { delay(1); }

  Serial.println("[Wio] Boot");
  Serial1.begin(UART_BAUD);                // STMとのUART
  pinMode(PIN_TRIG, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_TRIG), onTrigRise, RISING);

  setupNetwork();                          // DISABLE_NETWORKにより実行/スキップ
  Serial.println("[Wio] READY (waiting D30 rising)");
}

void loop(){
  if (!g_triggered){ delay(5); return; }
  g_triggered = false;

  Serial.println("\n[Wio] Triggered (D30↑). Send token 0xA5");
  flushInputQuick(Serial1);
  delay(20);                    // STMが受信待ちへ入る猶予
  Serial1.write(WAKE_TOKEN);    // ★ 1バイトトークン
  Serial.println("[Wio] -> 0xA5");

  // --- START <count> を待つ ---
  String line;
  if (!readLineWithDeadline(Serial1, line, millis()+START_TIMEOUT_MS)){
    Serial.println("[Wio] START timeout");
    return;
  }
  if (!line.startsWith("START ")){
    Serial.print("[Wio] Unexpected first line: "); Serial.println(line);
    return;
  }
  long count = line.substring(6).toInt();
  Serial.print("[Wio] START count="); Serial.println(count);

  unsigned long max_ts_good=0;
  unsigned long frames_ok=0;

  for (long i=0;i<count;i++){
    // --- TS= の同期をとる（崩れ復帰可） ---
    if (!syncToTS(Serial1, TS_TIMEOUT_MS)){
      Serial.println("[Wio] TS sync timeout");
      break;
    }
    unsigned long ts_hdr=0;
    if (!readTSnumber(Serial1, ts_hdr, TS_TIMEOUT_MS)){
      Serial.println("[Wio] TS read fail");
      break;
    }

    // --- 56HEXを読む（CR/LF/ゴミ無視で56桁） ---
    char hex56[57];
    if (!readHex56(Serial1, hex56, PAYLOAD_TIMEOUT_MS)){
      Serial.println("[Wio] HEX 56 read timeout/short");
      break;
    }

    // 変換
    uint8_t raw28[28];
    String hexLine(hex56);
    if (!hex56_to_bytes28(hexLine, raw28)){
      Serial.println("[Wio] HEX->BIN fail");
      continue;
    }
    const rec28_t* r = (const rec28_t*)raw28;

    // 検証
    uint16_t crc_calc = crc16_ccitt(raw28, 26);
    bool crc_ok = (crc_calc == r->crc16);
    bool ts_ok  = (r->ts == ts_hdr);

    Serial.print("[Wio] TS(hdr)="); Serial.print(ts_hdr);
    Serial.print(" TS(rec)=");       Serial.print(r->ts);
    Serial.print(" CRC=");           Serial.print(crc_ok?"OK":"NG");
    Serial.print(" TS=");            Serial.println(ts_ok?"OK":"NG");

    if (crc_ok && ts_ok){
      handle_good_frame_and_maybe_send(*r);   // ネットワークOFFでもJSON表示
      if (r->ts > max_ts_good) max_ts_good = r->ts;
      frames_ok++;
    }
  }

  // --- END を期待（無くてもACKは返す） ---
  if (readLineWithDeadline(Serial1, line, millis()+2000) && line=="END"){
    Serial.println("[Wio] END received");
  } else {
    Serial.println("[Wio] END missing (continue)");
  }

  // --- ACK 返信 ---
  if (max_ts_good>0){
    Serial1.print("ACK "); Serial1.println(max_ts_good);
    Serial.print("[Wio] -> ACK "); Serial.println(max_ts_good);
  } else {
    Serial1.println("ACK 0");
    Serial.println("[Wio] -> ACK 0");
  }

  Serial.print("[Wio] frames_ok="); Serial.println(frames_ok);
}
