#include "CredStore.h"
#include <EEPROM.h>

// ---- 設定 ----
static const uint16_t kEEPROM_SIZE = 512;   // 必要容量（512で十分）
static const uint16_t kEEPROM_ADDR = 0;     // 先頭に書く

// ---- レコード定義 ----
struct CredRecord {
  uint32_t magic;     // 'CRED'
  uint16_t version;   // 1
  char     ssid[32];  // ESP8266の実用上32で十分
  char     pass[64];  // パスワード
  uint32_t crc32;     // magic～pass までのCRC32
} __attribute__((packed));

static const uint32_t MAGIC = 0x44455243u; // 'C','R','E','D'の逆順(LE)
static const uint16_t VER   = 1;

// ---- CRC32（標準多項式0xEDB88320）----
static uint32_t crc32_update(uint32_t crc, uint8_t d) {
  crc ^= d;
  for (int i = 0; i < 8; ++i) {
    uint32_t m = -(crc & 1u);
    crc = (crc >> 1) ^ (0xEDB88320u & m);
  }
  return crc;
}
static uint32_t crc32_bytes(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = crc32_update(c, p[i]);
  return ~c;
}

// ---- 内部：実体の読み書き ----
static void eeprom_read(uint16_t addr, uint8_t* dst, size_t len) {
  for (size_t i = 0; i < len; ++i) dst[i] = EEPROM.read(addr + i);
}
static void eeprom_write(uint16_t addr, const uint8_t* src, size_t len) {
  for (size_t i = 0; i < len; ++i) EEPROM.write(addr + i, src[i]);
}

void CredStore_begin() {
  EEPROM.begin(kEEPROM_SIZE);
}

bool CredStore_load(String& outSsid, String& outPass) {
  CredRecord rec{};
  eeprom_read(kEEPROM_ADDR, reinterpret_cast<uint8_t*>(&rec), sizeof(rec));

  if (rec.magic != MAGIC || rec.version != VER) return false;

  // CRC確認
  const size_t body_len = sizeof(rec) - sizeof(rec.crc32);
  const uint32_t c = crc32_bytes(reinterpret_cast<const uint8_t*>(&rec), body_len);
  if (c != rec.crc32) return false;

  // C文字列保証
  rec.ssid[sizeof(rec.ssid)-1] = '\0';
  rec.pass[sizeof(rec.pass)-1] = '\0';

  outSsid = String(rec.ssid);
  outPass = String(rec.pass);
  return (outSsid.length() > 0);
}

bool CredStore_save(const String& ssid, const String& pass) {
  CredRecord rec{};
  rec.magic   = MAGIC;
  rec.version = VER;

  // 安全にコピー
  memset(rec.ssid, 0, sizeof(rec.ssid));
  memset(rec.pass, 0, sizeof(rec.pass));
  ssid.substring(0, sizeof(rec.ssid)-1).toCharArray(rec.ssid, sizeof(rec.ssid));
  pass.substring(0, sizeof(rec.pass)-1).toCharArray(rec.pass, sizeof(rec.pass));

  const size_t body_len = sizeof(rec) - sizeof(rec.crc32);
  rec.crc32 = crc32_bytes(reinterpret_cast<const uint8_t*>(&rec), body_len);

  eeprom_write(kEEPROM_ADDR, reinterpret_cast<const uint8_t*>(&rec), sizeof(rec));
  return EEPROM.commit();
}

void CredStore_clear() {
  // 先頭のmagicを消すだけで十分（高速）
  uint32_t zero = 0;
  eeprom_write(kEEPROM_ADDR, reinterpret_cast<const uint8_t*>(&zero), sizeof(zero));
  EEPROM.commit();
}
