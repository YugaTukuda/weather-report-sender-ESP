#pragma once
#include <Arduino.h>

// APを立ち上げ、設定用Webサーバ(80)を開始
void ConfigPortal_begin(const char* apSsid, const char* apPass);

// ループで呼ぶ。HTTPリクエスト処理など
void ConfigPortal_tick();

// 入力済みなら true
bool ConfigPortal_hasCredentials();

// 入力されたSSID/PASSを取得
void ConfigPortal_getCredentials(String& outSsid, String& outPass);

// APとWebサーバを停止
void ConfigPortal_end();
