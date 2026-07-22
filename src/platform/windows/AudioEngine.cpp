#include "platform/windows/AudioEngine.h"

#include "realtime/DeviceRouting.h"
#include "realtime/RealtimeAudioPipeline.h"
#include "realtime/RealtimeVoiceProcessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include <miniaudio.h>

// Windows must precede avrt because the latter uses core Win32 type definitions.
// clang-format off
#include <windows.h>
#include <avrt.h>
// clang-format on

namespace vvc::windows {
namespace {

constexpr ma_uint32 kSampleRate = 48'000U;

[[nodiscard]] std::wstring utf8ToWide(const char* input) {
  if (input == nullptr || input[0] == '\0') {
    return L"(名称なし)";
  }
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, nullptr, 0);
  const UINT codePage = count > 0 ? CP_UTF8 : CP_ACP;
  const DWORD flags = count > 0 ? MB_ERR_INVALID_CHARS : 0U;
  const int fallbackCount = MultiByteToWideChar(codePage, flags, input, -1, nullptr, 0);
  if (fallbackCount <= 1) {
    return L"(名称なし)";
  }
  std::wstring output(static_cast<std::size_t>(fallbackCount), L'\0');
  MultiByteToWideChar(codePage, flags, input, -1, output.data(), fallbackCount);
  output.resize(static_cast<std::size_t>(fallbackCount - 1));
  return output;
}

void storePeak(std::atomic<float>& destination, const std::span<const float> samples) {
  float peak = 0.0F;
  for (const float sample : samples) {
    peak = std::max(peak, std::abs(sample));
  }
  float previous = destination.load(std::memory_order_relaxed);
  while (peak > previous &&
         !destination.compare_exchange_weak(previous, peak, std::memory_order_relaxed)) {
  }
}

[[nodiscard]] std::wstring miniaudioError(const std::wstring_view prefix, const ma_result result) {
  std::wstring message(prefix);
  message += L" (miniaudio: ";
  message += utf8ToWide(ma_result_description(result));
  message += L")";
  return message;
}

[[nodiscard]] realtime::AudioFailureReason
notificationFailure(const ma_device_notification_type type) {
  switch (type) {
  case ma_device_notification_type_stopped:
    return realtime::AudioFailureReason::device_stopped;
  case ma_device_notification_type_rerouted:
    return realtime::AudioFailureReason::device_rerouted;
  case ma_device_notification_type_interruption_began:
    return realtime::AudioFailureReason::interruption_began;
  default:
    return realtime::AudioFailureReason::none;
  }
}

} // namespace

struct AudioEngine::Impl {
  Impl() {
    input_ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const std::array backends{ma_backend_wasapi};
    const ma_context_config config = ma_context_config_init();
    const ma_result result = ma_context_init(
        backends.data(), static_cast<ma_uint32>(backends.size()), &config, &context);
    context_initialized = result == MA_SUCCESS;
    if (!context_initialized) {
      context_error = miniaudioError(L"Windows音声システムを初期化できません", result);
    }
  }

  ~Impl() {
    stop();
    if (context_initialized) {
      ma_context_uninit(&context);
    }
    if (input_ready_event != nullptr) {
      CloseHandle(input_ready_event);
    }
  }

  bool refresh(std::wstring& errorMessage) {
    (void)serviceLifecycle();
    if (lifecycle.isRunning()) {
      errorMessage = L"動作中はデバイスを再読み込みできません。";
      return false;
    }
    if (!context_initialized) {
      errorMessage = context_error;
      return false;
    }

    ma_device_info* playback = nullptr;
    ma_uint32 playbackCount = 0U;
    ma_device_info* capture = nullptr;
    ma_uint32 captureCount = 0U;
    const ma_result result =
        ma_context_get_devices(&context, &playback, &playbackCount, &capture, &captureCount);
    if (result != MA_SUCCESS) {
      errorMessage = miniaudioError(L"マイクと出力先を取得できません", result);
      return false;
    }

    capture_native.clear();
    playback_native.clear();
    if (captureCount != 0U) {
      capture_native.assign(capture, capture + captureCount);
    }
    if (playbackCount != 0U) {
      playback_native.assign(playback, playback + playbackCount);
    }
    capture_public.clear();
    playback_public.clear();
    capture_public.reserve(captureCount);
    playback_public.reserve(playbackCount);
    for (const auto& info : capture_native) {
      capture_public.push_back({utf8ToWide(info.name), info.isDefault != 0U});
    }
    for (const auto& info : playback_native) {
      playback_public.push_back({utf8ToWide(info.name), info.isDefault != 0U});
    }
    (void)lifecycle.acknowledgeRecovery();
    return true;
  }

  AudioStartResult start(const std::size_t captureIndex, const std::size_t playbackIndex) {
    if (lifecycle.isRunning()) {
      return {true, L"すでに動作中です。"};
    }
    (void)serviceLifecycle();
    if (!context_initialized) {
      return {false, context_error};
    }
    if (input_ready_event == nullptr) {
      return {false, L"音声処理ワーカーの待機イベントを作成できません。"};
    }
    if (captureIndex >= capture_native.size() || playbackIndex >= playback_native.size()) {
      return {false, L"マイクまたは出力先を選び直してください。"};
    }
    if (!realtime::isSafeCaptureName(capture_public[captureIndex].name)) {
      return {false, L"上段は CABLE Output "
                     L"ではないマイク入力（物理マイク推奨）を選び、再度開始してください。"};
    }
    if (!realtime::isCablePlaybackName(playback_public[playbackIndex].name)) {
      return {false, L"出力先に CABLE Input を選び、再度開始してください。"};
    }
    if (!lifecycle.beginStart()) {
      return {false, L"音声デバイスを再読み込みしてから開始してください。"};
    }
    const realtime::AudioLifecycleGeneration sessionGeneration = lifecycle.snapshot().generation;

    try {
      processor.reset();
    } catch (...) {
      (void)lifecycle.fail(realtime::AudioFailureReason::dsp_failure, sessionGeneration);
      (void)serviceLifecycle();
      return {false, L"音声処理を初期化できません。再読み込み後に再試行してください。"};
    }
    pipeline.prepare();
    input_peak.store(0.0F, std::memory_order_relaxed);
    output_peak.store(0.0F, std::memory_order_relaxed);
    ResetEvent(input_ready_event);

    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.sampleRate = kSampleRate;
    config.periodSizeInFrames =
        static_cast<ma_uint32>(realtime::RealtimeAudioPipeline::kBlockFrames);
    config.periods = 3U;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = dataCallback;
    config.notificationCallback = notificationCallback;
    config.pUserData = this;
    config.playback.pDeviceID = &playback_native[playbackIndex].id;
    config.playback.format = ma_format_f32;
    config.playback.channels = 1U;
    config.capture.pDeviceID = &capture_native[captureIndex].id;
    config.capture.format = ma_format_f32;
    config.capture.channels = 1U;

    const ma_result initResult = ma_device_init(&context, &config, &device);
    if (initResult != MA_SUCCESS) {
      (void)lifecycle.fail(realtime::AudioFailureReason::device_open_failed, sessionGeneration);
      (void)serviceLifecycle();
      return {false,
              miniaudioError(L"選択した音声デバイスを開けません。再接続後に再読み込みしてください",
                             initResult)};
    }
    device_initialized = true;
    worker_active.store(true, std::memory_order_release);
    try {
      worker = std::thread([this, sessionGeneration] { processLoop(sessionGeneration); });
    } catch (...) {
      worker_active.store(false, std::memory_order_release);
      (void)lifecycle.fail(realtime::AudioFailureReason::device_start_failed, sessionGeneration);
      (void)serviceLifecycle();
      return {false, L"音声処理ワーカーを開始できません。再読み込み後に再試行してください。"};
    }

    const ma_result startResult = ma_device_start(&device);
    if (startResult != MA_SUCCESS) {
      (void)lifecycle.fail(realtime::AudioFailureReason::device_start_failed, sessionGeneration);
      (void)serviceLifecycle();
      return {false,
              miniaudioError(L"音声ストリームを開始できません。再読み込み後に再試行してください",
                             startResult)};
    }
    if (!lifecycle.markRunning(sessionGeneration)) {
      (void)lifecycle.fail(realtime::AudioFailureReason::device_start_failed, sessionGeneration);
      (void)serviceLifecycle();
      return {false, L"音声ストリームの開始中に中断されました。再読み込み後に再試行してください。"};
    }

    return {true, L"変換中です。通話アプリの入力は「CABLE Output」を選んでください。"};
  }

  void cleanupResources() noexcept {
    pipeline.suspend();
    worker_active.store(false, std::memory_order_release);
    if (input_ready_event != nullptr) {
      SetEvent(input_ready_event);
    }
    if (device_initialized) {
      ma_device_stop(&device);
    }
    // The UI thread is the sole cleanup owner and never calls this from the DSP worker, so a
    // joinable thread always satisfies std::thread::join's preconditions.
    if (worker.joinable()) {
      worker.join();
    }
    if (device_initialized) {
      ma_device_uninit(&device);
      device_initialized = false;
    }
  }

  void stop() noexcept {
    lifecycle.stop();
    cleanupResources();
  }

  AudioServiceResult serviceLifecycle() {
    bool cleanupPerformed = false;
    if (lifecycle.consumeCleanupRequest()) {
      cleanupResources();
      cleanupPerformed = true;
    }
    const auto state = lifecycle.snapshot();
    return {state.state, state.failure, cleanupPerformed};
  }

  static void dataCallback(ma_device* device, void* output, const void* input,
                           const ma_uint32 frameCount) noexcept {
    if (device == nullptr) {
      return;
    }
    auto* self = static_cast<Impl*>(device->pUserData);
    if (output == nullptr) {
      return;
    }
    auto outputSamples = std::span(static_cast<float*>(output), frameCount);
    if (self == nullptr || !self->lifecycle.isRunning()) {
      std::fill(outputSamples.begin(), outputSamples.end(), 0.0F);
      return;
    }

    if (input != nullptr) {
      const auto inputSamples =
          std::span(static_cast<const float*>(input), static_cast<std::size_t>(frameCount));
      storePeak(self->input_peak, inputSamples);
      (void)self->pipeline.pushInput(inputSamples);
    }
    (void)self->pipeline.popOutput(outputSamples);
    storePeak(self->output_peak, outputSamples);
    if (self->input_ready_event != nullptr) {
      SetEvent(self->input_ready_event);
    }
  }

  static void notificationCallback(const ma_device_notification* notification) noexcept {
    if (notification == nullptr || notification->pDevice == nullptr) {
      return;
    }
    auto* self = static_cast<Impl*>(notification->pDevice->pUserData);
    if (self == nullptr) {
      return;
    }
    const auto lifecycleSnapshot = self->lifecycle.snapshot();
    if (lifecycleSnapshot.state != realtime::AudioLifecycleState::starting &&
        lifecycleSnapshot.state != realtime::AudioLifecycleState::running) {
      return;
    }
    const auto failure = notificationFailure(notification->type);
    if (failure != realtime::AudioFailureReason::none &&
        self->lifecycle.fail(failure, lifecycleSnapshot.generation)) {
      self->pipeline.suspend();
      self->worker_active.store(false, std::memory_order_release);
      if (self->input_ready_event != nullptr) {
        SetEvent(self->input_ready_event);
      }
    }
  }

  void processLoop(const realtime::AudioLifecycleGeneration sessionGeneration) {
    DWORD taskIndex = 0U;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss != nullptr) {
      (void)AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    }

    while (worker_active.load(std::memory_order_acquire)) {
      const bool processed = pipeline.processOne(
          [this](const std::span<const float> input, const std::span<float> output) {
            processor.processBlock(input, output);
          },
          [this] { processor.reset(); });
      if (pipeline.hasFatalFailure()) {
        if (lifecycle.fail(realtime::AudioFailureReason::dsp_failure, sessionGeneration)) {
          worker_active.store(false, std::memory_order_release);
        }
        break;
      }
      if (!processed && worker_active.load(std::memory_order_acquire)) {
        const DWORD waitResult = WaitForSingleObject(input_ready_event, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
          if (lifecycle.fail(realtime::AudioFailureReason::dsp_failure, sessionGeneration)) {
            pipeline.suspend();
            worker_active.store(false, std::memory_order_release);
          }
          break;
        }
      }
    }

    if (mmcss != nullptr) {
      (void)AvRevertMmThreadCharacteristics(mmcss);
    }
  }

  ma_context context{};
  ma_device device{};
  HANDLE input_ready_event = nullptr;
  bool context_initialized = false;
  bool device_initialized = false;
  std::wstring context_error;
  std::vector<ma_device_info> capture_native;
  std::vector<ma_device_info> playback_native;
  std::vector<AudioDeviceInfo> capture_public;
  std::vector<AudioDeviceInfo> playback_public;
  realtime::AudioLifecycle lifecycle;
  realtime::RealtimeAudioPipeline pipeline;
  realtime::RealtimeVoiceProcessor processor;
  std::thread worker;
  std::atomic<bool> worker_active{false};
  std::atomic<float> input_peak{0.0F};
  std::atomic<float> output_peak{0.0F};
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() = default;

bool AudioEngine::refreshDevices(std::wstring& errorMessage) {
  return impl_->refresh(errorMessage);
}

const std::vector<AudioDeviceInfo>& AudioEngine::captureDevices() const {
  return impl_->capture_public;
}

const std::vector<AudioDeviceInfo>& AudioEngine::playbackDevices() const {
  return impl_->playback_public;
}

AudioStartResult AudioEngine::start(const std::size_t captureIndex,
                                    const std::size_t playbackIndex) {
  return impl_->start(captureIndex, playbackIndex);
}

void AudioEngine::stop() noexcept { impl_->stop(); }

AudioServiceResult AudioEngine::service() { return impl_->serviceLifecycle(); }

void AudioEngine::setVoiceSettings(const dsp::VoiceSettings settings) {
  impl_->processor.setVoiceSettings(settings);
}

void AudioEngine::setBypassed(const bool bypassed) { impl_->processor.setBypassed(bypassed); }
void AudioEngine::setMuted(const bool muted) { impl_->processor.setMuted(muted); }
bool AudioEngine::isRunning() const { return impl_->lifecycle.isRunning(); }

AudioStats AudioEngine::takeStats() {
  const int latencyMs = static_cast<int>(
      std::lround(1'000.0 * impl_->processor.algorithmLatencySamples() / kSampleRate));
  const auto pipeline = impl_->pipeline.stats();
  return {impl_->input_peak.exchange(0.0F, std::memory_order_relaxed),
          impl_->output_peak.exchange(0.0F, std::memory_order_relaxed),
          pipeline.input_overruns,
          pipeline.output_underruns,
          pipeline.deadline_misses,
          pipeline.resynchronizations,
          pipeline.fatal_dsp_failures,
          pipeline.input_depth,
          pipeline.output_depth,
          pipeline.max_input_depth,
          pipeline.max_output_depth,
          pipeline.avoidable_backlog,
          pipeline.max_avoidable_backlog,
          latencyMs};
}

} // namespace vvc::windows
