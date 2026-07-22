# ESP32-S3 tethering client

ESP32-S3 が deep sleep から起床するたびにスマートフォンのテザリングへ接続し、HTTPS サーバーへリクエストを送ります。結果はオンボード LED の点滅で通知し、次の接続試行までの周期が約 180 秒になるように deep sleep 時間を動的に調整して再スリープします。起床要因はタイマーとオンボードボタンのどちらでも可能です。

このコードは Heltec WB32L を起点にして作っていますが、手元の Heltec 個体では表示部が壊れているため、Heltec 用の表示機能は使っていません。表示用の電源は起動時に OFF にしたまま固定し、状態通知は LED のみで行います。

利用手順の草案は `USER_GUIDE.md` を参照してください。JC4827W543C_I での初期設定、AP mode への入り方、通常運用、トラブル時の見方をまとめています。

## 設定

```sh
idf.py set-target esp32s3
idf.py menuconfig
idf.py build flash monitor
```

`Tethering client configuration` で SSID、パスワード、HTTPS URL、ステータス LED の GPIO を設定してください。URL は公開 CA の HTTPS エンドポイントを想定しています（証明書バンドルを使用）。

`Board profile` で対象基板を選びます。

- 既定値は JC4827W543C_I です。
- Heltec WB32LAF: GPIO35 をステータス LED、GPIO36 を表示部電源、GPIO0 を BOOT ボタン、SX1262 無効化を既定にします。
- ESP32-C3 board: GPIO8 をステータス LED、表示部電源は無効、BOOT ボタンは無効を既定にします。ESP32-C3 は deep sleep の起床に使える GPIO が 0〜5 に限られるため、ボタン wake を使うなら RTC 対応 GPIO に変更してください。
- JC4827W543C_I: 4.3inch のタッチ付き LCD を持つ ESP32-S3 ボードです。SW1 は GPIO0 の deep sleep wake-up 専用ボタンとして使います。LCD はタイムスタンプ付きのログ表示に使い、Wi-Fi 設定は AP mode のブラウザ設定ページから行います。

Heltec WiFi LoRa 32 / WB32L（V3系）向けの既定値はステータス LED が GPIO35、アクティブ Low です。別のボードや別配線では変更してください。

BOOT ボタンは既定で GPIO0（Low で押下）です。GPIO0 が RTC GPIO として使える ESP32-S3 開発ボード向けです。ボタンの GPIO や論理レベルが異なる場合は `Button wakeup GPIO` と `Button active level` を変更してください。JC4827W543C_I では SW1 をこの wake-up ボタンとして使い、通常の Wi-Fi 設定操作には使いません。deep sleep 中にボタンを押したままにすると、起床直後に再度 sleep に入った場合もすぐ起床するため、押して離してください。

## Heltec WB32L の LoRa 無効化

`Keep onboard SX1262 LoRa radio disabled` は既定で有効です。起動直後に SX1262 の RESET（GPIO12）を Low に保持し、NSS（GPIO8）を High、SCK/MOSI（GPIO9/10）を Low にするため、LoRa 通信は開始されません。これは V3 系のピン配置を対象にしています。

SX1262 の電源はボードの 3.3 V 電源レールから供給され、ファームウェアで個別に電源遮断する回路ではありません。そのためこの設定は「論理的に無効化（RESET 保持）」です。厳密な電源遮断が必要なら、3.3 V 電源経路のハードウェア変更が必要です。

## 動作

1. 起床後、Wi-Fi 接続を開始します。
2. 接続できれば HTTPS GET を一度実行します。
3. 成功なら LED を 200 ms 点灯して消灯します。
4. 失敗なら LED を 200 ms 点灯 / 200 ms 消灯で 5 回繰り返してから消灯します。
5. 処理が終わったら、次の接続試行までの周期が約 180 秒になるよう timer wakeup 秒数を調整して deep sleep に入ります。

Wi-Fi 接続待ちの上限は `menuconfig` の timeout で変更できます。初期設定のままでは SSID が空なので通信は行いません。

## テザリング再接続試験

1. スマホでテザリングを有効にし、SSID・パスワード・通信先 URL を設定して書き込みます。
2. LED の点灯パターンで `HTTP 200 OK` 相当の成功/失敗を確認します。サーバー側のアクセスログも併用すると確実です。
3. おおむね 180 秒周期での timer 起床、または wake-up ボタンによる起床後に、再び `WIFI CONNECTED` が出るか確認します。JC4827W543C_I では SW1 がこの wake-up ボタンです。
4. ESP が sleep に入った後、スマホをロックして同じ試験を繰り返します。スマホの省電力設定でテザリングが停止する場合は LED の失敗パターンになります。

失敗を切り分けるには USB シリアルログ（`idf.py monitor`）を取り、スマホのテザリング設定で「画面オフ時もテザリングを維持」または省電力除外に相当する項目を確認してください。機種や OS のポリシーによって、画面ロック中の再接続可否は異なります。

フラッシュ運用メモ（JC4827W543C_I）:

- deep sleep に入るとタイミングによってはファーム書き込みしづらくなります。
- AP の選択画面を開いたままにしておくと deep sleep に入らないため、その間は書き込みできます。

## 実験結果

2026-07-16 時点で、iPhone テザリングに対して「起床 → Wi-Fi 接続 → HTTPS GET → 成否を LED 表示 → 約 180 秒周期で再試行」を 1 時間連続で繰り返し、安定して動作することを確認しました。

## 対応ボード

### Heltec WB32LAF (ESP32-S3 with LoRa)

このプロジェクトでは、Heltec WB32LAF 系で次の GPIO を使います。

- ステータス LED: GPIO35
- 表示部の電源制御: GPIO36
- 起床ボタン: GPIO0
- SX1262 無効化用の保持信号:
  - NSS: GPIO8
  - SCK: GPIO9
  - MOSI: GPIO10
  - RESET: GPIO12

白いオンボード LED はステータス表示に使います。オレンジ系の LED は電源/充電系の表示で、このプログラムでは制御しません。

### JC4827W543C_I

この基板は 4.3 インチのタッチ付き LCD を持ちます。

- SW1 wake ボタン: GPIO0
- LCD バックライト: GPIO1
- LCD quad bus:
  - CS: GPIO45
  - CLK: GPIO47
  - D0: GPIO21
  - D1: GPIO48
  - D2: GPIO40
  - D3: GPIO39
- Touch GT911:
  - SDA: GPIO8
  - SCL: GPIO4
  - INT: GPIO3
  - RST: 未使用

このリポジトリでは、JC4827W543C_I では LCD にテザリング接続結果と HTTP 成否をタイムスタンプ付きで表示します。ログ表示はタッチで上下にスクロールでき、Wi-Fi 設定画面への遷移は画面上の `Select` ボタンで行います。SW1 は deep sleep からの wake-up 専用です。LCD の reset ピンは未接続前提です。配線やモジュール差で LCD が初期化できない場合は `menuconfig` でピンを調整してください。

### ESP32-C3 + 0.42 inch OLED

boot button: GPIO9
onboard LED: GPIO8
OLED SDA: GPIO5
OLED SCL: GPIO6

## ToDo

- JC4827W543 の充電管理 IC と SW1 の関係を調査する（SW1 が電源キー入力か、GPIO 連動が可能かを確認）
- deep sleep からの復帰は当面 BOOT ボタン運用のままとする
