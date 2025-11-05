🛰️ CCBT_wifi — STM連携・Wi-Fi送信・DeepSleep制御 (for ESP8266)
概要

CCBT_wifi.ino は、ESP8266（例: AE-ESP-WROOM02-DEV） を用いて
1️⃣ 起動（RST起床）
2️⃣ STMとのUART通信（Wio互換プロトコル）
3️⃣ Wi-Fi経由でJSON送信（UDP）
4️⃣ DeepSleepで待機
を自動実行するスケッチです。

STMから受け取ったセンサデータなどをPCへWi-Fi送信し、
バッテリー消費を最小限に抑えるIoT中継モジュールとして動作します。

🔧 開発環境
項目	内容
ボード	AE-ESP-WROOM02-DEV（ESP8266）
Arduino IDE ボードマネージャーURL	http://arduino.esp8266.com/stable/package_esp8266com_index.json
ボード設定例	[ツール] → [ボード] → ESP8266 Boards → "Generic ESP8266 Module"
フラッシュサイズ	4MB (FS: 1MB OTA: 1019KB) 推奨
CPU周波数	80MHz または 160MHz
書き込み速度	115200bps

秋月の技術資料（AE-ESP-WROOM02-DEV）:
👉 https://akizukidenshi.com/goodsaffix/AE-ESP-WROOM02-DEV.pdf

📡 動作フロー
[1] 電源ON or RST起床
        ↓
[2] 設定済みSSID/PASSがあればSTM通信
     なければConfigPortal(AP)起動
        ↓
[3] STMとUART通信でJSONデータ受信
        ↓
[4] Wi-Fi接続 → UDPでPCへ送信
        ↓
[5] DeepSleep(無期限) → 次回RSTで再起動

📶 初回セットアップ手順

ESP8266を電源ON
→ SSIDが ESP8266_SETUP のアクセスポイントが出現

PC/スマホで接続し、設定画面からWi-FiのSSIDとパスワードを登録

登録情報は CredStore に保存され、次回起動から自動接続します。

🔌 配線（UART）
ESP8266	STM側	備考
TX (GPIO1)	RX	UART通信（Wio互換）
RX (GPIO3)	TX	同上
GND	GND	共通グラウンド

※USBシリアルと競合する場合は Serial.swap() を使用可。

📤 送信仕様

宛先：255.255.255.255:5005（ブロードキャスト）

プロトコル：UDP

内容：STMから受信したJSONを自動分割して送信

例:

{"data":[{"time":1234,"temp":24.3,"humid":50.2}, ...]}

💤 スリープ仕様

送信完了後に ESP.deepSleep(0) を実行し、
外部RST信号でのみ復帰します（低消費電力設計）。

🧩 関連ヘッダ構成（参考）
ファイル名	概要
ConfigPortal.h	Wi-Fi設定用ポータル（APモード）
CredStore.h	SSID/PASS保存領域
STMProtocol.h	STMとのJSON通信処理
UdpOnce.h	UDP送信ユーティリティ
⚙️ 定数変更ポイント
定義	説明	例
DEST_IP_OCTx	送信先IP（ブロードキャスト or 固定IP）	192,168,11,5
DEST_PORT	送信先ポート番号	5005
AP_SSID / AP_PASS	設定用APのSSID/PASS	"ESP8266_SETUP"
STM_BAUD	STM通信速度	19200
