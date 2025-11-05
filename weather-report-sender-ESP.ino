// CCBT_wifi.ino — RST起床→STMとWio互換通信→Wi-Fi送信→DeepSleep
#include <ESP8266WiFi.h>
#include "ConfigPortal.h"
#include "CredStore.h"
#include "STMProtocol.h"
#include "UdpOnce.h"

// ======= 送信先（PC側） =======
#define DEST_IP_OCT1 255
#define DEST_IP_OCT2 255
#define DEST_IP_OCT3 255
#define DEST_IP_OCT4 255
#define DEST_PORT    5005

// ======= AP初期設定 =======
#define AP_SSID  "ESP8266_SETUP"
#define AP_PASS  "esp8266ap"   // 8+ chars

// ======= UART（STMと接続） =======
// NOTE: 開発キットでUSBと競合する場合はログを減らすか、配線を合わせてください。
// 必要に応じて Serial.swap() 等の運用に変更可。
#define STM_BAUD 19200

enum class AppState { BOOT, CONFIG_AP, SESSION, WIFI_SEND, SLEEP };
static AppState g_state = AppState::BOOT;

static String g_ssid, g_pass;
static String g_json;  // {"data":[ {...}, ... ]}

static void go_deepsleep_forever() {
  Serial.println("[PWR] DeepSleep forever (wake by external RST)");
  delay(50);
  ESP.deepSleep(0); // 無期限 → RST起床
}

void setup() {
  Serial.begin(19200);
  delay(20);
  Serial.println("\n[BOOT] Wio-compatible session on ESP8266");

  CredStore_begin();

  if (CredStore_load(g_ssid, g_pass)) {
    Serial.print("[CRED] SSID="); Serial.print(g_ssid);
    Serial.print(" PASS="); Serial.println(g_pass.length()?"********":"(empty)");
    g_state = AppState::SESSION;  // 直ちにSTMと通信
  } else {
    Serial.println("[CRED] not found → Config AP");
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
        Serial.print("[CFG] SSID="); Serial.print(g_ssid);
        Serial.print(" PASS="); Serial.println(g_pass.length()?"********":"(empty)");
        if (CredStore_save(g_ssid, g_pass)) {
          Serial.println("[CRED] saved");
        } else {
          Serial.println("[CRED] save failed");
        }
        ConfigPortal_end();
        // 初期セットアップ完了 → 次回からRST起床で使うため即DeepSleep
        g_state = AppState::SLEEP;
      }
      break;
    }

    case AppState::SESSION: {
      // --- STMとWio互換セッション ---
      Serial.println("[STM] session start");
      Serial.flush();
      Serial.begin(STM_BAUD);

      STM_SessionResult res{};
      if (!STM_run_session(Serial, res)) {
        Serial.println("[STM] session failed → sleep");
        g_state = AppState::SLEEP;
        break;
      }
      // JSON完成
      g_json = res.jsonPayload;
      Serial.print("[STM] ok, records="); Serial.println(res.okCount);
      Serial.print("[STM] max_ts_good="); Serial.println(res.maxTsGood);

      g_state = AppState::WIFI_SEND;
      break;
    }

    case AppState::WIFI_SEND: {
      WiFi.mode(WIFI_STA);
      Serial.print("[WiFi] connect "); Serial.println(g_ssid);
      WiFi.begin(g_ssid.c_str(), g_pass.c_str());

      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(200);
        Serial.print(".");
      }
      Serial.println();

      if (WiFi.status() == WL_CONNECTED) {
        IPAddress dest(DEST_IP_OCT1, DEST_IP_OCT2, DEST_IP_OCT3, DEST_IP_OCT4);

        // ★まとめ送信（必要なら自動分割）
        bool ok = UDP_SEND_JSON_AGGREGATED(dest, DEST_PORT, g_json, /*mtu_payload=*/1200);
        Serial.print("[UDP] aggregated send "); Serial.println(ok ? "OK" : "FAIL");
      } else {
        Serial.println("[WiFi] connect FAIL (skip send)");
      }

      g_state = AppState::SLEEP;
      break;
    }


    case AppState::SLEEP: {
      go_deepsleep_forever();
      break; // ここには戻らない
    }

    case AppState::BOOT:
    default: break;
  }
}
