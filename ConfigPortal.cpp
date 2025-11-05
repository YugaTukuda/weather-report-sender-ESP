#include "ConfigPortal.h"
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

namespace {
  ESP8266WebServer server(80);
  bool credsReady = false;
  String ssid, pass;

  const char* PAGE_FORM =
R"HTML(<!doctype html>
<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>ESP Wi-Fi Setup</title>
<style>
body{font-family:sans-serif;max-width:520px;margin:24px auto;padding:0 12px}
label{display:block;margin:12px 0 6px} input{width:100%;padding:8px}
button{margin-top:16px;padding:10px 16px}
.code{font-family:monospace;background:#f3f3f3;padding:8px;border-radius:6px}
</style></head>
<body>
<h2>ESP8266 Wi-Fi 設定</h2>
<p>現在は <b>APモード</b> です。PC/スマホをこのAPに接続した状態で、下のフォームに接続先Wi-FiのSSIDとパスワードを入力してください。</p>
<form method="POST" action="/save">
  <label>SSID</label>
  <input name="ssid" placeholder="Your Wi-Fi SSID" required>
  <label>Password</label>
  <input name="pass" placeholder="Your Wi-Fi Password" type="password">
  <button type="submit">保存して接続</button>
</form>
<hr>
<p class="code">このページ: http://192.168.4.1/</p>
</body></html>)HTML";

  const char* PAGE_OK =
R"HTML(<!doctype html><meta charset='utf-8'>
<body style="font-family:sans-serif">
<h3>設定を受け付けました。</h3>
<p>APを停止し、指定のWi-Fiへ接続します。<br>
シリアルモニタ(115200bps)で進捗をご確認ください。</p>
</body>)HTML";

  void handleRoot() {
    server.send(200, "text/html; charset=utf-8", PAGE_FORM);
  }

  void handleSave() {
    ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
    pass = server.hasArg("pass") ? server.arg("pass") : "";
    credsReady = (ssid.length() > 0);
    server.send(200, "text/html; charset=utf-8", PAGE_OK);
  }
}

void ConfigPortal_begin(const char* apSsid, const char* apPass){
  credsReady = false; ssid = ""; pass = "";

  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(apSsid, apPass);  // 192.168.4.1
  delay(100); // AP起動待ち
  Serial.print("[AP] SSID="); Serial.print(apSsid);
  Serial.print(" PASS="); Serial.print(apPass);
  Serial.print(" IP=");   Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("[HTTP] config portal started at http://192.168.4.1/");
}

void ConfigPortal_tick(){
  server.handleClient();
  // （必要ならここでWiFiスキャン等の追加機能も実装可）
}

bool ConfigPortal_hasCredentials(){
  return credsReady;
}

void ConfigPortal_getCredentials(String& outSsid, String& outPass){
  outSsid = ssid;
  outPass = pass;
}

void ConfigPortal_end(){
  server.stop();
  delay(50);
  WiFi.softAPdisconnect(true);
  delay(50);
  Serial.println("[AP] stopped. switching to STA...");
}
