# Ticker Firmware Design (Ticker mode + AP mode)

## 1. 目的

このファームウェアに、以下の2モードを持たせる。

- Ticker mode:
  - 既存のティッカー動作（STA接続 -> Web API取得 -> 画面更新 -> 待機/Deep Sleep）
- AP mode:
  - Wi-Fi AP + HTTPサーバーを起動し、ブラウザから設定を編集

要求の優先事項:

- AP mode中はTicker mode機能を止めてよい
- Deep Sleepを止めてよい
- RTC保持データを維持しなくてよい
- AP modeの起動トリガーは BOOT スイッチ長押し（3秒以上）

## 2. 期待効果

- タッチパネル非搭載LCD構成でも設定導線を維持できる
- SSID/パスワード/API URL入力をスマホで行えるためUX向上
- 排他モード化により、実行時RAMピークを抑えやすい

## 3. 前提と制約

現時点の観測値（size出力）:

- App image: 約1.285MB
- App partition残: 約251KB
- DIRAM残: 約119KB
- IRAM残: ほぼ0KB
- RTC SLOW残: ほぼ0KB

設計制約:

- IRAM負荷を増やさない（IRAM_ATTR追加を避ける）
- 設定用HTTPはローカル用途のため非TLS
- HTML/JSは最小（1ファイルまたは2ファイル）

ハードウェア仕様未確定時の方針:

- 当面は AP mode起動スイッチを BOOT ボタンに固定して実装する
- BOOT長押し（3秒以上）でAP modeへ遷移
- BOOT短押し（3秒未満）は通常wake-upとしてTicker modeへ遷移

## 4. モード設計

## 4.1 モード定義

```c
typedef enum {
  APP_MODE_TICKER = 0,
  APP_MODE_AP,
} app_mode_t;
```

## 4.2 遷移

- Ticker -> AP
  - 条件A: BOOTスイッチを3秒以上長押し
- AP -> Ticker
  - 条件A: HTTP設定保存成功後、`esp_restart()`
  - 条件B: ユーザーがExit操作、`esp_restart()`

長押し判定仕様:

- 判定対象GPIO: `CONFIG_TETHER_WAKE_BUTTON_GPIO`（既定はGPIO0/BOOT）
- Active level: `CONFIG_TETHER_WAKE_BUTTON_ACTIVE_LEVEL`
- 長押し時間: 3000ms（固定）
- 判定タイミング: 起動直後（app_main初期化フェーズ）
- 誤判定防止: 50ms周期でレベル確認し、連続3秒成立でAP modeへ遷移
- 3秒未満で離された場合はTicker modeで通常起動する
- Deep Sleep中の押下要件:
  - Deep Sleep中でもBOOT押下で即wake-up（`EXT0`）すること
  - `EXT0` wake後、BOOT押下が継続している場合は長押し判定を継続し、3秒成立でAP modeへ遷移
  - これにより「タイマー起床を待たずに」AP modeへ入れることを保証

再起動を使う理由:

- サブシステム停止/再開の複雑性を下げる
- メモリ断片化リスクを抑える
- 既存コードへの侵襲を最小化

## 5. AP modeのランタイム方針

AP modeでは以下を止める/使わない。

- STA接続フロー
- HTTPクライアント処理
- Deep Sleep遷移
- RTC保持ロジック（依存しない）

AP modeで動かすもの:

- Wi-Fi AP（softAP）
- DNS（captive portal用途、必須）
- HTTP server（esp_http_server）

Captive Portal方針:

- AP modeでは captive portal を必須機能として実装する
- DNSクエリは原則すべてAP自身のIPへ解決させる
- HTTPアクセスは設定トップページ（`/`）へ誘導する
- OSの自動接続判定URL（Android/iOS/Windows想定）にも応答し、設定ページへ到達可能にする

初期導線QR方針（実装したい機能）:

- AP mode起動時にQRを表示し、スマホからの初期接続導線を短縮する
- 最低限のQR内容:
  - Wi-Fi接続情報（SSID / password / security）
  - 設定URL（例: `http://192.168.4.1/`）
- 表示順序:
  - 1) `SETUP AP STARTING...`
  - 2) AP情報表示（SSID / IP）
  - 3) QR表示
- タッチなしLCD構成でも、QR読み取りだけで設定画面に到達できることを目標にする

AP modeタイムアウト方針（2026-07-22時点）:

- 本日時点では実装しない
- 要検討事項としてドキュメント管理のみ行う
- 将来の検討観点:
  - 無操作タイムアウト（例: 5分/10分）
  - 保存処理中のタイムアウト抑止
  - タイムアウト時の復帰先（Ticker mode再起動 or AP mode維持）

## 6. 設定データモデル

NVS namespace例: `cfg`

- `sta_ssid` (string, max 32)
- `sta_pass` (string, max 64)
- `api_url` (string, max 256)
- `poll_sec` (u32, default 180)
- `tz` (string, optional)

互換:

- 既存 `wifi_cfg` namespaceの読み取り互換レイヤを残す
- 新規保存は `cfg` に統一

A/Bスロット拡張（任意、余裕があれば実装）:

- 本フェーズでは必須要件にしない
- 将来、設定を `slot_a` / `slot_b` で保持し、`active_slot` で選択する
- 新設定を未検証スロットに保存し、疎通確認後に `active_slot` を切替
- 連続失敗時に旧スロットへロールバックする余地を残す

## 6.1 ログ保存方針（試作段階）

- 試作段階ではログ保存先をSDメモリカードにしてよい
- 画面表示用の短期ログ（リングバッファ）は従来どおりRAM保持
- 永続ログはSDカードへCSV追記
- SD未挿入時はRAMログのみで継続し、機能停止しない
- AP mode中の設定変更ログも同一フォーマットで追記可能にする

CSVフォーマット（固定）:

- ヘッダ:
  `ts,mode,event,ssid,rssi,http_status,reason,detail`
- 例:
  `2026-07-22T09:12:30+09:00,TICKER,WIFI_CONNECTED,iPhoneXR++,-58,,0,sta connected`
  `2026-07-22T09:12:33+09:00,TICKER,HTTP_OK,iPhoneXR++,-58,200,0,request success`
  `2026-07-22T09:20:00+09:00,AP,CONFIG_SAVED,,,,0,save and restart`

ログローテーション（試作向け簡易）:

- ファイル名は日単位（例: `log_YYYYMMDD.txt`）
- 1ファイル上限サイズを設定値で制御（例: 1MB）
- 上限超過時は新規ファイルへ切り替え

## 7. HTTP API/画面仕様（最小）

## 7.1 エンドポイント

- `GET /`
  - 設定画面HTMLを返す
- `GET /generate_204`
  - captive portal誘導用（Android系想定）
- `GET /hotspot-detect.html`
  - captive portal誘導用（Apple系想定）
- `GET /ncsi.txt`
  - captive portal誘導用（Windows系想定）
- `GET /api/config`
  - 現在設定JSON
- `POST /api/config`
  - 設定保存（バリデーションあり）
- `POST /api/restart`
  - 200応答後に再起動
- `GET /api/health`
  - 稼働確認

## 7.2 画面項目

- STA SSID
- STA Password
- API URL
- Poll interval(sec)
- 通信状態を音で知らせる（ON/OFF、デフォルトON）
- Saveボタン
- Save and Restartボタン

実装済みUI挙動（現時点）:

- ページ表示時に `GET /api/config` で現在値をロード
- `保存` ボタンで `POST /api/config` 実行（再起動しない）
- `保存して再起動` ボタンで `POST /api/config` 成功後に `POST /api/restart` 実行
- 設定画面はカード型レイアウト（モバイル優先）で表示し、トグルUIで音通知設定を切り替える
- 画面上部に 3 ステップの案内を表示:
  - テザリング SSID / パスワードを入力する
  - `保存して再起動` を押す
  - 再起動後にスマホのインターネット共有ページを開いたままにする
- `保存` 実行後は「AP mode のまま」であることを明示する
- `保存して再起動` 実行中はボタンを無効化し、AP 切断が起きる旨を表示する

`/api/config` JSONスキーマ（現実装）:

```json
{
  "sta_ssid": "string(1..32)",
  "sta_pass": "string(1..64)",
  "api_url": "http(s)://...",
  "poll_sec": 180,
  "sound_enabled": true
}
```

保存先（NVS）:

- namespace: `cfg`
- keys: `sta_ssid`, `sta_pass`, `api_url`, `poll_sec`, `sound_enabled`
- 互換用途として `wifi_cfg` (`ssid`,`pass`) にも同期保存

音通知設定の意味:

- `sound_enabled=true`: 接続開始音 / 成功音 / 失敗音を鳴らす
- `sound_enabled=false`: ブザー音をすべて抑止する（画面ログ・LEDは継続）

## 7.3 バリデーション

- SSID: 1..32
- Password: 8..64（open運用を許す場合は0可を要件化）
- URL: `https://` または `http://` を要件に応じて許可
- poll_sec: 30..86400

## 7.4 条件付きGET（ETag / If-Modified-Since）対応

目的:

- AP modeの設定画面や設定JSONを、差分なし時は `304 Not Modified` で返し、通信量と描画待ち時間を削減する
- captive portalの再アクセス連打時に、不要な本文再送を抑える

対象候補:

- `GET /`（設定画面HTML）
- `GET /api/config`（現在設定JSON）
- captive portal判定用エンドポイント（`/generate_204` など）は原則キャッシュ抑止とし、条件付きGETは必須にしない

レスポンスヘッダ方針:

- 条件付きGETを受けるリソースには `ETag` と `Last-Modified` の両方を付与する
- 追加で `Cache-Control` を明示する
  - 設定JSON: `Cache-Control: no-cache`（毎回再検証させる）
  - 静的HTML: `Cache-Control: no-cache` または短い `max-age`（PoCでは `no-cache` 推奨）

ETag生成方針:

- 強いETagより、軽量な弱いETag（例: `W/\"<rev>-<len>\"`）を優先
- 候補キー:
  - 設定データの更新カウンタ（NVS更新時に+1）
  - もしくは内容ハッシュ（FNV-1a/CRC32など軽量）
- 小規模SoC向けに、毎回全文SHA計算は避ける

Last-Modified生成方針:

- 設定保存成功時刻（UTC）を保持し、RFC 7231形式（IMF-fixdate）で返す
- SNTP未同期時は時刻信頼性が低いため、以下いずれかを採用する
  - A) `Last-Modified` を省略して `ETag` のみで判定
  - B) ビルド時刻/起動時刻ベースの疑似時刻を使う（誤判定リスクを明記）

判定優先順位（実装ルール）:

- クライアントが `If-None-Match` を送ってきた場合はETag判定を優先
- `If-Modified-Since` は `If-None-Match` が無い場合のみ評価
- 一致時は `304 Not Modified` を返し、本文は送らない
- 不一致時は `200 OK` + 本文 + 最新 `ETag` / `Last-Modified`

実装時の注意:

- `304` 応答でも `ETag` と `Cache-Control` は返す
- 設定更新直後は必ず新しいETagを発行し、古いキャッシュを無効化する
- AP mode中の同時更新を考慮し、ETag計算に使う値の読み取りを排他する
- URLごとにキャッシュポリシーを分ける（`/api/config` と `/` を同一扱いしない）

テスト観点:

- 初回 `GET /api/config` が `200` で `ETag` を返す
- 同じ `ETag` を `If-None-Match` で送ると `304`
- 設定更新後に旧 `ETag` で再取得すると `200` + 新 `ETag`
- `If-Modified-Since` 単独送信時の `200/304` 判定が仕様どおり
- SNTP未同期環境でも誤って304固定にならない

## 8. セキュリティ方針（PoC段階）

最小実装:

- APの初期パスワード固定（例: `ticker-setup`）
- 設定画面に簡易PIN（4〜8桁）を追加可能にする余地

将来:

- APパスワードをNVS管理
- 設定画面にCSRF対策トークン

## 9. メモリ最適化ガイド

- `httpd_config_t` を小さく設定
  - `max_open_sockets = 2`
  - `max_uri_handlers = 8` 程度
  - task stackはデフォルトより小さい安全値へ調整（要実測）
- レスポンスは固定バッファで生成
- 大きなテンプレートエンジン不使用
- JSONパーサは軽量実装（cJSONを使うならオブジェクト数を最小化）

## 10. UI設計（既存ログ画面との統合）

- 既存 `Select` に加え `Settings` ボタンを追加
- AP mode起動時:
  - 画面に `SETUP AP STARTING...` 表示
  - その後 `SETUP AP: SSID=... IP=...` 表示
  - QR表示（SSID/接続URL）

タッチなしLCD構成では BOOT長押し起動を標準導線とする。

## 11. 実装ステップ（推奨順）

1. 設定ストア抽象化
   - `config_store.c/.h` を作成
   - NVS load/save API統一
2. モードステートマシン導入
   - `app_mode_t` と遷移ハンドラ
3. AP起動/停止モジュール
   - `wifi_ap_mode.c/.h`
4. HTTP serverモジュール
  - `ap_http_server.c/.h`
5. Captive portalモジュール
  - DNSリダイレクト実装
  - 自動接続判定URLハンドラ実装
6. QR表示モジュール
  - AP接続情報QRの生成と描画
7. BOOT長押し判定実装
  - 3秒以上でAP mode遷移
  - `ESP_SLEEP_WAKEUP_EXT0` からの復帰時も同じ判定経路を使う
8. 保存後再起動フロー
9. エラー表示・ログ整理
10. サイズ再測定

## 12. 受け入れ条件

- AP modeでスマホから接続できる
- AP接続後、ブラウザ起動だけで設定ページへ到達できる（captive portal）
- AP mode画面のQRを読み取るだけで設定ページへ到達できる
- SSID/Password/API URL保存が成功する
- 再起動後Ticker modeで保存値が使われる
- Ticker modeでは現行の180秒サイクル挙動を維持
- ビルド警告なしを維持
- アプリサイズがパーティション上限を超えない

## 13. リスクと対策

- リスク: AP+HTTP追加でFlash不足
  - 対策: HTML最小化、機能段階導入、不要ログ削減
- リスク: 設定モードから戻る時の状態不整合
  - 対策: 保存後は必ず再起動
- リスク: 接続不能時のサポートコスト
  - 対策: 画面にAP名/IPを明示表示

## 14. この設計の判断

- 実装可能性: 高い
- メモリ成立性: 条件付きで高い（軽量HTTP前提）
- UX: 現状より改善見込み大
- コスト: タッチ依存を外せるため改善余地が大きい

## 15. 不採用とした案

- 二段階セーフモード（連続失敗時に自動でAP modeへ遷移）は不採用
  - 理由: テザリングは一時的に切断されることがあり、ユーザーがWi-Fi共有の設定ページを開けば復旧可能なため
  - 判断: 自動遷移よりも、通常のTicker mode継続を優先する
