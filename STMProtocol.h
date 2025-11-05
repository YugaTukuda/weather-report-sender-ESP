#pragma once
#include <Arduino.h>

// CRC16種別（必要に応じて切替）
enum class CRC16Mode : uint8_t { MODBUS_IBM = 0, CCITT_1021 = 1 };

struct STM_SessionResult {
  String jsonPayload;   // {"data":[ {...}, ... ]}
  uint32_t maxTsGood = 0;
  uint32_t okCount   = 0;
  uint32_t badCount  = 0;
};

// Wio互換プロトコルを1セッション実行
// 戻り値: 成功/失敗
bool STM_run_session(Stream& io, STM_SessionResult& out,
                     CRC16Mode mode = CRC16Mode::MODBUS_IBM,
                     uint32_t line_timeout_ms = 2000);

// 汎用ヘルパ（必要なら他所でも使用可）
bool readLineWithDeadline(Stream& s, String& out, uint32_t deadline_ms);
