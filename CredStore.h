#pragma once
#include <Arduino.h>

// 永続保存API
void    CredStore_begin();                                   // EEPROM初期化
bool    CredStore_load(String& outSsid, String& outPass);    // 読み出し（成功= true）
bool    CredStore_save(const String& ssid, const String& pass); // 保存（成功= true）
void    CredStore_clear();                                   // 消去（次回は未設定扱い）
