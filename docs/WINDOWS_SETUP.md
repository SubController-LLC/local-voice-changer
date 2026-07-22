# Windowsセットアップ

## 1. VB-CABLEを入れる

1. [VB-Audio公式のVB-CABLEページ](https://vb-audio.com/Cable/)からWindows版を取得します。
2. ZIPを展開し、64-bit Windowsでは`VBCABLE_Setup_x64.exe`を管理者として実行します。
3. `Install Driver`を実行し、Windowsを再起動します。
4. Windowsの「設定 → システム → サウンド → その他のサウンド設定」で、再生側に`CABLE Input`、録音側に`CABLE Output`があることを確認します。

VB-CABLEは、`CABLE Input`に再生した音を`CABLE Output`から録音デバイスとして取り出す仮想ケーブルです。ドライバーは本リポジトリやZIPに同梱しません。

## 2. アプリを開始する

1. [GitHub Releases](https://github.com/SubController-LLC/local-voice-changer/releases)から公開ベータのZIPと`SHA256SUMS.txt`を取得し、ZIPを展開します。チェックサムで改ざんがないことを追加確認できます。
2. `LocalVoiceChanger.exe`を起動します。Windows SmartScreenが表示される場合があります。現在のベータ版はAuthenticode署名されていません。
3. 上段で普段使う物理マイクを選びます。
4. 下段で`CABLE Input (VB-Audio Virtual Cable)`を選びます。
5. 「変換を開始」を押します。Naturalの推奨設定が自動適用され、マイクと出力のメーターが動きます。

上段へ`CABLE Output`を選ぶと仮想ケーブル内で音声がループするため、アプリは開始を拒否します。

## 3. 通話アプリへ渡す

通話アプリまたはゲームの入力デバイスに`CABLE Output (VB-Audio Virtual Cable)`を選び、利用できる場合はループバックで音量を確認します。

## トラブル対応

### CABLE Input / Outputが出ない

- VB-CABLEインストール後にWindowsを再起動します。
- Windowsの「その他のサウンド設定」で無効になっていないか確認します。
- アプリの「再読み込み」を押します。

### マイクメーターが動かない

- Windowsの「設定 → プライバシーとセキュリティ → マイク」で、デスクトップアプリのマイクアクセスを許可します。
- Discord、OBS、DAWなどがマイクを排他モードで開いている場合は終了します。

### 通話アプリに音が届かない

- アプリの出力メーターが動いていることを確認します。
- 通話アプリの入力が`CABLE Output`か再確認します。`CABLE Input`と反対なのが正しい設定です。
- Windowsで`CABLE Input`と`CABLE Output`の既定の形式を48 kHzに揃えると、機種依存の変換トラブルを減らせます。

### 音がブツブツ切れる

- ステータス下の`resync`または`deadline miss`が増えるか確認します。
- 重い配信、録画、音声加工アプリを一時的に止めます。
- 改善しない場合、デバイスの48 kHz設定とドライバーを確認します。

### ハウリングする

Windowsの`CABLE Output`で「このデバイスを聴く」を有効にしないでください。試聴は通話アプリのループバックまたはヘッドホンを使った別の録音ツールで行います。
