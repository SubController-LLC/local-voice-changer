# 検証方針と現在の状態

## 自動検証の証拠

CIが成功したコミットについてのみ、次の自動証拠があると扱います。ローカルビルドだけで配布ZIPを検証済みとは表示しません。

- C++フォーマット
- macOS DebugとASan/UBSanでのDSP、WAV、bounded pipeline、lifecycle、routing、CLIテスト
- Windows Server 2022 x64 Releaseビルドと同じテスト
- 配布ZIPを新しいフォルダへ展開し、ZIP内の`LocalVoiceChanger.exe --self-test`を実行
- 同じEXEで非表示のWin32画面を作り、汎用タイトル、必須コントロール、削除済み高度調整、安全な初期状態を検査するスモークテスト
- PE32+ x64 GUI形式、静的CRT、VERSIONINFO、埋め込みmanifest、必要ファイル、プライバシー禁止物、SHA-256
- タグ、CMake、VERSIONINFO、manifest、パッケージ検証、リリースノートのバージョン一致
- 自動検査を通過したタグ版ZIPのGitHub Artifact Attestation

自己診断は実際の配布EXEにリンクされたbounded pipeline、3,000-sample DSP遅延、lifecycle、routing、設定読込、WASAPI列挙probeを通ります。Audio ServiceやendpointがないCIでは、WASAPIのエラー表示経路を検査します。これにより「コアのテストだけ成功し、配布EXEは起動しない」という見落としを防ぎますが、物理ストリームの開始成功までは証明しません。

## 自動検証では保証できないこと

GitHubのWindows環境には、利用者の物理マイク、VB-CABLE、実際の通話アプリやゲーム負荷がありません。そのため、次は実機ゲートです。

- Windows 10と11でのマイク → CABLE Input → CABLE Output
- 通話アプリのループバックとゲーム互換性経路
- 端末ごとの自然さ、ノイズ、総往復遅延
- デバイス抜去、排他利用、スリープ復帰
- 60分の音切れ、クロックずれ、CPU負荷試験
- SmartScreenを含む初回ダウンロード体験

配布ZIPそのものが上記の自動検査を通過した場合に限り、公開ベータをGitHubの`prerelease`として配布します。実機項目が未完了であることをREADMEとリリースノートへ明記し、「全環境で問題なし」「安定版」とは表示しません。安定版へ昇格するには[Windows QA](WINDOWS_QA.md)の実機記録が必要です。

CIのプライバシー検査は現在の追跡ソースと配布ZIPを対象にします。公開リポジトリは非公開の旧履歴を引き継がないクリーンなルートコミットから開始し、追跡ファイルに個人メール、秘密鍵、認証トークン、音声、設定、ログがないことを検査します。

## 遅延の定義

Signalsmith処理は50 ms窓、12.5 ms間隔、分割計算を使い、報告するアルゴリズム遅延はinputとoutputの合計3,000 samples（48 kHzで62.5 ms）です。バイパス原音も同じ3,000 samples遅らせます。

ランタイムキューは通常480 framesをプライミングし、それを超える回避可能バックログを960 frames（20 ms）以下に保ちます。上限到達時は古い音声を再生せず、無音化して再同期します。

WASAPI、ドライバー、VB-CABLEのバッファは別に加わります。実機ではマイクからCABLE Outputまでのp95を120 ms以下を目標、150 ms以上をNo-Goとします。

## 実機結果の記録

[Windows QA](WINDOWS_QA.md)の項目を端末ごとに記録し、個人の音声ファイルやデバイス固有IDはリポジトリへ追加しません。音質比較を行う場合は[ブラインド試聴手順](LISTENING_TEST.md)を使います。
