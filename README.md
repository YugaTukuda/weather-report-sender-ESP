# 🛰️ CCBT_wifi — STM連携・Wi-Fi送信・DeepSleep制御 (for ESP8266)

## 概要
**CCBT_wifi.ino** は、ESP8266（例: [AE-ESP-WROOM02-DEV](https://akizukidenshi.com/goodsaffix/AE-ESP-WROOM02-DEV.pdf)）を用いて  
1️⃣ **起動（RST起床）**  
2️⃣ **STMとのUART通信（Wio互換プロトコル）**  
3️⃣ **Wi-Fi経由でJSON送信（UDP）**  
4️⃣ **DeepSleepで待機**  
を自動実行するスケッチです。

STMから受け取ったセンサデータなどをWi-Fi経由でPCへ送信し、  
**低消費電力IoT中継モジュール**として動作します。

---

## 🔧 開発環境

| 項目 | 内容 |
|------|------|
| ボード | AE-ESP-WROOM02-DEV（ESP8266） |
| Arduino IDE 追加ボードマネージャーURL | `http://arduino.esp8266.com/stable/package_esp8266com_index.json` |
| ボード設定 | **[ツール] → [ボード] → ESP8266 Boards → "Generic ESP8266 Module"** |
| フラッシュサイズ | 4MB (FS: 1MB OTA: 1019KB) 推奨 |
| CPU周波数 | 80MHz または 160MHz |
| 書き込み速度 | 115200bps |

📘 秋月技術資料 → [AE-ESP-WROOM02-DEV.pdf](https://akizukidenshi.com/goodsaffix/AE-ESP-WROOM02-DEV.pdf)

---

## 📡 動作フロー

```mermaid
graph TD;
    A[起動/RST] --> B{保存済みWi-Fi情報?};
    B -- あり --> C[STMとUART通信];
    B -- なし --> D[ConfigPortal(APモード)];
    C --> E[Wi-Fi接続];
    E --> F[UDPでJSON送信];
    F --> G[DeepSleep];
    D --> G;
```

---

## 📶 初回セットアップ手順

1. ESP8266を電源ON  
   → SSID `ESP8266_SETUP` のアクセスポイントが出現  
2. PCまたはスマホで接続し、ブラウザからWi-FiのSSIDとパスワードを登録  
3. 設定情報はフラッシュに保存され、次回起動から自動接続します。

---

## 🔌 配線（UART）

| ESP8266 | STM側 | 備考 |
|----------|--------|------|
| TX (GPIO1) | RX | UART通信（Wio互換） |
| RX (GPIO3) | TX | 同上 |
| GND | GND | 共通グラウンド |

> 💡 USBシリアルと競合する場合は `Serial.swap()` を使用してください。

---

## 📤 送信仕様

- **宛先:** `255.255.255.255:5005`（UDPブロードキャスト）
- **プロトコル:** UDP  
- **送信内容:** STMから受信したJSON文字列  
- **送信方法:** `UDP_SEND_JSON_AGGREGATED(dest, port, json, mtu_payload=1200)`

例:
```json
{"data":[{"time":1234,"temp":24.3,"humid":50.2}, ...]}
```

---

## 💤 スリープ仕様

送信完了後に `ESP.deepSleep(0)` を実行し、  
**外部RST信号でのみ復帰可能**です。  
（超低消費電力での長期稼働を想定）

---

## 🧩 関連ヘッダ構成

| ファイル名 | 概要 |
|-------------|------|
| `ConfigPortal.h` | Wi-Fi設定用ポータル（APモード） |
| `CredStore.h` | SSID/PASSの保存管理 |
| `STMProtocol.h` | STMとのJSON通信処理 |
| `UdpOnce.h` | UDP送信ユーティリティ |

---

## ⚙️ 定数変更ポイント

| 定義 | 内容 | 例 |
|------|------|------|
| `DEST_IP_OCTx` | 送信先IP | `192,168,11,5` |
| `DEST_PORT` | UDPポート番号 | `5005` |
| `AP_SSID` / `AP_PASS` | 設定用アクセスポイント | `"ESP8266_SETUP" / "esp8266ap"` |
| `STM_BAUD` | STM通信速度 | `19200` |

---

## 🧾 ライセンス

MIT License  
(c) 2025 Yuga Tsukuda / CCBT Project

---
