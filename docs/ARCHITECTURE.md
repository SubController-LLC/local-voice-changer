# Architecture

## 実行時データフロー

`LocalVoiceChanger.exe`はWASAPIデバイスとして利用者が選んだマイク入力をcapture、VB-CABLEの`CABLE Input`をplaybackにしたduplexストリームを開きます。通話アプリはWindowsが公開する反対側のcaptureデバイス`CABLE Output`を読みます。

```text
WASAPI callback                    DSP worker
------------------------------    ----------------------------------------
mic f32 mono -> input SPSC FIFO -> Signalsmith pitch + formant shift
                                    -> 80 Hz HPF / gate / compressor
                                    -> -1 dBFS soft limiter
CABLE Input <- output SPSC FIFO <- 20 ms bypass/mute crossfade
```

## リアルタイム保護

- 48 kHz mono、10 msの固定DSPブロックを使います。スペクトル窓は50 ms、間隔は12.5 msで、アルゴリズム総遅延は62.5 msです。
- callbackはバウンド済みSPSCパイプラインの転送、ピーク計測、アンダーラン時の0埋めだけを行います。
- callback中にmutex、動的メモリ確保、ログ、ファイルI/O、UI更新、デバイス操作はありません。
- 通常の出力プライミングは480 framesです。入出力キューの回避可能バックログが960 frames（20 ms）を超える前に古い入力と出力を各consumerが破棄し、無音、DSP reset、480-frame再プライミングの順で再同期します。
- 入力のNaN/Infを0へ置換し、変換、バイパス、ミュート、過渡中の最終出力を-1 dBFS以下に保護します。
- バイパス原音にもinput+outputを合計したDSP遅延を与え、切り替えは20 msでクロスフェードします。
- DSPワーカーはWindows MMCSSの`Pro Audio`へ登録し、通常のバックグラウンドスレッドよりスケジューリングを優先します。
- callbackは自動リセットイベントでDSPワーカーを起こし、Windows timer粒度に依存するpollingを行いません。
- キュー深度の現在値/最大値、10 ms期限違反、再同期、underrun/overrun、致命的DSP失敗をメモリ上だけで記録します。
- DSP例外またはデバイス中断は処理継続ではなく、無音化とリソース解放を一度だけ実行するrecoverable errorへ遷移します。

## モジュール

- `core/audio`: オフライン検証用の境界チェック付きWAV I/Oとメトリクス。
- `core/dsp`: オフライン音質比較、フォルマント候補の回帰検証、セーフティチェーン。
- `realtime/RealtimeVoiceProcessor`: 状態を保つDSP、パラメータ平滑化、遅延を揃えたバイパス。
- `realtime/AudioRingBuffer`: cache lineを分けたlock-free SPSC FIFO。
- `realtime/RealtimeAudioPipeline`: キュー上限、再同期ハンドシェイク、ランタイム診断。
- `realtime/AudioLifecycle`: stopped / starting / running / recoverable errorのスレッドセーフな状態モデル。
- `realtime/DeviceRouting`: 列挙名に基づく`CABLE Output`入力の拒否と`CABLE Input`出力の選択規則。
- `platform/windows/AudioEngine`: miniaudio/WASAPI、duplex callback、DSP worker。
- `platform/windows/app_main`: 日本語Win32 UI、デバイス選択、レベル表示、セットアップ案内。
- `platform/windows/AppSettings`: `%LOCALAPPDATA%` のJSON設定。ミュートや自動起動は永続しません。

## フォルマント変換の範囲

Signalsmith Stretch 1.3.2のスペクトルピッチ変換と専用フォルマント変換を同時に使います。リアルタイム処理は重いスペクトル計算を複数ブロックに分割し、ワーカースレッドの負荷スパイクを抑えます。上流に1.3.2のリリースタグがないため、同バージョンを報告するコミット`57b93f4e`とアーカイブSHA-256を固定しています。

これは話者特定、音声クローン、学習モデルではありません。公開ベータ版のUIは検証対象のNatural（`+3.0` pitch、`+2.0` formant）だけを使います。

## ビルド保護

依存はタグURLとSHA-256で固定し、MSVCは`/W4 /WX /permissive- /utf-8 /guard:cf`、macOSは警告をエラー化します。WindowsはCRTを静的リンクし、単体EXEを作ります。CIは配布ZIP内のEXEを実行し、manifestとPE形式まで検証します。
