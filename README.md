# Local Voice Changer

Windows 10/11でマイクをリアルタイムに女の子寄りの声へ変換し、VB-CABLE経由で通話アプリへ渡す、ローカル完結のオープンソースアプリです。

> **公開ベータ版:** 配布ZIPとその中のEXEはWindows CIで自動検査しますが、物理マイク、VB-CABLE、ゲーム内ボイスチャットを通した実機試験と60分試験は未完了です。安定版ではなく、機種ごとの動作や声質を保証するものではありません。

- 検証対象を Natural（pitch `+3.0`、formant `+2.0`）に絞った一つの声
- 列挙されたデバイス名を検査し、入力側の`CABLE Output`と出力側の非`CABLE Input`を拒否するループ防止
- 3,000 samples（62.5 ms）に揃えた変換音とバイパス原音
- 古い音声を溜めず、20 msの回避可能バックログで再同期するパイプライン
- ノイズゲート、コンプレッサー、`-1 dBFS`リミッター
- 原音比較とミュートの20 msクロスフェード
- デバイス中断とDSP失敗の安全停止、再読み込み案内
- 録音、テレメトリ、クラウド通信、音声クローンなし

## 公開ベータ版のセットアップ

1. [GitHub Releases](https://github.com/SubController-LLC/local-voice-changer/releases)から`LocalVoiceChanger-Windows-x64.zip`と`SHA256SUMS.txt`を取得します。
2. [VB-CABLE](https://vb-audio.com/Cable/)を公式サイトからインストールし、Windowsを再起動します。
3. ZIPを展開して`LocalVoiceChanger.exe`を起動します。インストーラーと管理者権限は不要です。
4. 物理マイクと`CABLE Input (VB-Audio Virtual Cable)`を選び、「変換を開始」を押します。
5. 通話アプリの入力デバイスに`CABLE Output`を選びます。

詳細は[Windowsセットアップ](docs/WINDOWS_SETUP.md)を参照してください。EXEはAuthenticode未署名のため、SmartScreenが表示される場合があります。

## 自動検証と未実施の実機確認

Windows CIが成功したコミットについて、次を自動検査します。

- 新しいフォルダへ展開した配布EXEの`--self-test`と`--ui-smoke-test`
- キュー上限、再同期、ライフサイクル、ルーティング、DSP、WAV、CLIのテスト
- PE32+ x64 GUI形式、VERSIONINFO、カスタムmanifest、同梱物、SHA-256
- CMake、タグ、配布EXE、manifest、リリースノートのバージョン一致
- macOS DebugとASan/UBSan、Windows x64 Release

自動検査は物理マイクやドライバーを証明しません。公開版は`prerelease`として配布し、未実施項目を隠しません。証拠の境界は[検証方針](docs/VERIFICATION.md)、実機項目は[Windows QA](docs/WINDOWS_QA.md)に分けています。

## リアルタイム構成

```text
物理マイク
  → WASAPI 48 kHz mono
  → bounded lock-free pipeline
  → pitch + spectral-envelope shift
  → high-pass + gate + compressor + limiter
  → CABLE Input → CABLE Output
  → voice chat
```

音声コールバックではロック、メモリ確保、ファイル処理、UI更新を行いません。通話アプリやゲームのプロセス、メモリ、ファイルにはアクセスしません。

## ビルド

Windows x64:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release
ctest --preset windows-release --output-on-failure
./scripts/package-windows.ps1
./scripts/verify-windows-package.ps1
```

初回configure時だけ、CMakeがSHA-256固定済みの依存アーカイブを取得します。配布EXEは通信しません。

## 安全な使い方

- 元声、話し方、マイクによって結果は変わり、性別や年齢の完全な変換は保証しません。
- 固有人物の声の複製、なりすまし、嫌がらせには利用しないでください。
- 利用するサービスの最新ルールとボイスチャットマナーに従ってください。
- VB-CABLEは同梱していません。必ず公式配布元から取得してください。

本プロジェクトはMIT Licenseです。依存ライセンスは[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)、セキュリティ報告方法は[SECURITY.md](SECURITY.md)を参照してください。
