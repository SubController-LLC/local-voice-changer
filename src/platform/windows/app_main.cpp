#include "platform/windows/AppSettings.h"
#include "platform/windows/AudioEngine.h"
#include "platform/windows/SelfTest.h"
#include "realtime/DeviceRouting.h"
#include "realtime/VoicePresets.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Windows must precede commctrl because the latter relies on core Win32 type definitions.
// clang-format off
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
// clang-format on

namespace {

constexpr wchar_t kWindowClass[] = L"SubControllerLocalVoiceChangerWindow";
constexpr wchar_t kWindowTitle[] = L"Local Voice Changer";
constexpr UINT_PTR kMeterTimer = 1U;
constexpr UINT_PTR kSmokeTestTimer = 2U;
constexpr int kCaptureCombo = 101;
constexpr int kPlaybackCombo = 102;
constexpr int kRefreshButton = 103;
constexpr int kVoiceSummary = 109;
constexpr int kRemovedCuteButton = 111;
constexpr int kRemovedClearButton = 112;
constexpr int kRemovedPitchSlider = 120;
constexpr int kRemovedFormantSlider = 121;
constexpr int kStartButton = 130;
constexpr int kBypassCheckbox = 131;
constexpr int kMuteCheckbox = 132;
constexpr int kInputMeter = 140;
constexpr int kOutputMeter = 141;
constexpr int kStatusText = 150;
constexpr int kHealthText = 151;

enum class UiState { needs_setup, ready, starting, running, stopped, recoverable_error };

[[nodiscard]] int meterValue(const float peak) {
  const float decibels = 20.0F * std::log10(std::max(peak, 0.000'001F));
  return static_cast<int>(
      std::lround(std::clamp((decibels + 60.0F) / 60.0F, 0.0F, 1.0F) * 1'000.0F));
}

class MainWindow {
public:
  explicit MainWindow(const HINSTANCE instance, const bool smokeTest)
      : instance_(instance), smoke_test_(smokeTest), settings_(vvc::windows::loadAppSettings()) {}

  ~MainWindow() {
    audio_.stop();
    if (font_ != nullptr) {
      DeleteObject(font_);
    }
    if (title_font_ != nullptr) {
      DeleteObject(title_font_);
    }
    if (background_brush_ != nullptr) {
      DeleteObject(background_brush_);
    }
  }

  bool create() {
    background_brush_ = CreateSolidBrush(RGB(20, 22, 28));
    if (background_brush_ == nullptr) {
      return false;
    }
    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = background_brush_;
    windowClass.lpszClassName = kWindowClass;
    if (RegisterClassExW(&windowClass) == 0U) {
      return false;
    }

    window_ = CreateWindowExW(
        0U, kWindowClass, kWindowTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 880, 640, nullptr, nullptr, instance_, this);
    if (window_ == nullptr) {
      return false;
    }
    ShowWindow(window_, smoke_test_ ? SW_HIDE : SW_SHOW);
    UpdateWindow(window_);
    return true;
  }

private:
  static LRESULT CALLBACK windowProcedure(const HWND window, const UINT message,
                                          const WPARAM wParam, const LPARAM lParam) noexcept {
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    try {
      if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        if (create == nullptr || create->lpCreateParams == nullptr) {
          return FALSE;
        }
        self = static_cast<MainWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      }
      return self != nullptr ? self->handleMessage(message, wParam, lParam)
                             : DefWindowProcW(window, message, wParam, lParam);
    } catch (...) {
      if (self != nullptr) {
        self->smoke_exit_code_ = 4;
      }
      PostQuitMessage(4);
      if (message == WM_NCCREATE) {
        return FALSE;
      }
      return message == WM_CREATE ? -1 : 0;
    }
  }

  LRESULT handleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
      if (!buildInterface()) {
        return -1;
      }
      audio_.setVoiceSettings(vvc::realtime::kNaturalVoiceSettings);
      audio_.setBypassed(settings_.bypassed);
      SendMessageW(bypass_checkbox_, BM_SETCHECK, settings_.bypassed ? BST_CHECKED : BST_UNCHECKED,
                   0);
      refreshDevices();
      if (SetTimer(window_, kMeterTimer, 50U, nullptr) == 0U) {
        return -1;
      }
      if (smoke_test_) {
        smoke_exit_code_ = verifySmokeInterface() ? 0 : 3;
        if (SetTimer(window_, kSmokeTestTimer, 100U, nullptr) == 0U) {
          return -1;
        }
      }
      return 0;
    case WM_COMMAND:
      handleCommand(LOWORD(wParam), HIWORD(wParam));
      return 0;
    case WM_TIMER:
      if (wParam == kMeterTimer) {
        updateMetersAndLifecycle();
      } else if (wParam == kSmokeTestTimer) {
        KillTimer(window_, kSmokeTestTimer);
        DestroyWindow(window_);
      }
      return 0;
    case WM_CTLCOLORSTATIC: {
      const HDC deviceContext = reinterpret_cast<HDC>(wParam);
      SetTextColor(deviceContext, RGB(235, 238, 245));
      SetBkColor(deviceContext, RGB(20, 22, 28));
      return reinterpret_cast<LRESULT>(background_brush_);
    }
    case WM_CLOSE:
      persistSettings();
      audio_.stop();
      DestroyWindow(window_);
      return 0;
    case WM_DESTROY:
      KillTimer(window_, kMeterTimer);
      PostQuitMessage(smoke_test_ ? smoke_exit_code_ : 0);
      return 0;
    default:
      return DefWindowProcW(window_, message, wParam, lParam);
    }
  }

  HWND addControl(const wchar_t* className, const wchar_t* text, const DWORD style, const int x,
                  const int y, const int width, const int height, const int id = 0) {
    HWND control = CreateWindowExW(
        0U, className, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (control == nullptr) {
      controls_valid_ = false;
      return nullptr;
    }
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return control;
  }

  void addLabel(const wchar_t* text, const int x, const int y, const int width, const int height,
                const bool title = false, const int id = 0) {
    HWND label = addControl(L"STATIC", text, SS_LEFT, x, y, width, height, id);
    if (title && label != nullptr) {
      SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_), TRUE);
    }
  }

  [[nodiscard]] bool buildInterface() {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
    font_ = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    title_font_ = CreateFontW(-28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (font_ == nullptr || title_font_ == nullptr) {
      return false;
    }

    addLabel(L"女の子寄りボイスチェンジャー", 28, 20, 600, 42, true);
    addLabel(L"端末内処理 / 録音・送信なし / 独立した非公式ツール", 30, 61, 700, 25);

    addLabel(L"1  マイク入力（物理マイク推奨）", 30, 102, 330, 26);
    capture_combo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 30, 130, 570, 280,
                                kCaptureCombo);
    addLabel(L"2  変換後の出力先（CABLE Input）", 30, 174, 400, 26);
    playback_combo_ = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 30, 202, 570,
                                 280, kPlaybackCombo);
    refresh_button_ =
        addControl(L"BUTTON", L"再読み込み", BS_PUSHBUTTON, 620, 202, 210, 34, kRefreshButton);

    addLabel(L"声: Natural（推奨設定 +3.0 pitch / +2.0 formant）", 30, 259, 700, 30, false,
             kVoiceSummary);
    start_button_ =
        addControl(L"BUTTON", L"変換を開始", BS_DEFPUSHBUTTON, 30, 306, 260, 48, kStartButton);
    bypass_checkbox_ = addControl(L"BUTTON", L"原音と比較（バイパス）", BS_AUTOCHECKBOX, 315, 310,
                                  260, 38, kBypassCheckbox);
    mute_checkbox_ =
        addControl(L"BUTTON", L"ミュート", BS_AUTOCHECKBOX, 610, 310, 160, 38, kMuteCheckbox);

    addLabel(L"マイク", 30, 382, 90, 24);
    input_meter_ = addControl(PROGRESS_CLASSW, L"", PBS_SMOOTH, 120, 384, 270, 18, kInputMeter);
    addLabel(L"出力", 435, 382, 70, 24);
    output_meter_ = addControl(PROGRESS_CLASSW, L"", PBS_SMOOTH, 505, 384, 325, 18, kOutputMeter);
    SendMessageW(input_meter_, PBM_SETRANGE32, 0, 1'000);
    SendMessageW(output_meter_, PBM_SETRANGE32, 0, 1'000);
    SendMessageW(input_meter_, PBM_SETBARCOLOR, 0, RGB(78, 201, 140));
    SendMessageW(output_meter_, PBM_SETBARCOLOR, 0, RGB(105, 156, 255));

    status_text_ = addControl(L"STATIC", L"準備中…", SS_LEFT, 30, 429, 800, 58, kStatusText);
    health_text_ = addControl(L"STATIC", L"", SS_LEFT, 30, 492, 800, 50, kHealthText);
    addLabel(L"通話アプリでは入力デバイスに CABLE Output を選びます。\r\n"
             L"ハウリング防止のため、スピーカーへのモニター機能はありません。",
             30, 550, 800, 50);
    return controls_valid_;
  }

  void refreshDevices() {
    renderState(UiState::starting, L"音声デバイスを再読み込み中です…");
    std::wstring error;
    if (!audio_.refreshDevices(error)) {
      renderState(UiState::recoverable_error,
                  error + L" 接続を確認して「再読み込み」を押してください。");
      return;
    }
    SendMessageW(capture_combo_, CB_RESETCONTENT, 0, 0);
    SendMessageW(playback_combo_, CB_RESETCONTENT, 0, 0);
    for (const auto& device : audio_.captureDevices()) {
      std::wstring label = device.name;
      if (device.is_default) {
        label += L"  [既定]";
      }
      SendMessageW(capture_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    for (const auto& device : audio_.playbackDevices()) {
      std::wstring label = device.name;
      if (device.is_default) {
        label += L"  [既定]";
      }
      SendMessageW(playback_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    selectSavedOrRecommendedDevices();
    renderRouteReadiness();
  }

  void selectSavedOrRecommendedDevices() {
    std::vector<vvc::realtime::RoutingDevice> capture;
    std::vector<vvc::realtime::RoutingDevice> playback;
    capture.reserve(audio_.captureDevices().size());
    playback.reserve(audio_.playbackDevices().size());
    for (const auto& device : audio_.captureDevices()) {
      capture.push_back({device.name, device.is_default});
    }
    for (const auto& device : audio_.playbackDevices()) {
      playback.push_back({device.name, device.is_default});
    }
    const auto captureSelection =
        vvc::realtime::chooseSafeCapture(capture, settings_.capture_device);
    const auto playbackSelection =
        vvc::realtime::chooseCablePlayback(playback, settings_.playback_device);
    SendMessageW(capture_combo_, CB_SETCURSEL,
                 captureSelection.has_value() ? static_cast<WPARAM>(*captureSelection)
                                              : static_cast<WPARAM>(-1),
                 0);
    SendMessageW(playback_combo_, CB_SETCURSEL,
                 playbackSelection.has_value() ? static_cast<WPARAM>(*playbackSelection)
                                               : static_cast<WPARAM>(-1),
                 0);
  }

  [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> selectedSafeRoute() const {
    const LRESULT capture = SendMessageW(capture_combo_, CB_GETCURSEL, 0, 0);
    const LRESULT playback = SendMessageW(playback_combo_, CB_GETCURSEL, 0, 0);
    if (capture == CB_ERR || playback == CB_ERR || capture < 0 || playback < 0 ||
        static_cast<std::size_t>(capture) >= audio_.captureDevices().size() ||
        static_cast<std::size_t>(playback) >= audio_.playbackDevices().size()) {
      return std::nullopt;
    }
    const std::size_t captureIndex = static_cast<std::size_t>(capture);
    const std::size_t playbackIndex = static_cast<std::size_t>(playback);
    if (!vvc::realtime::isSafeCaptureName(audio_.captureDevices()[captureIndex].name) ||
        !vvc::realtime::isCablePlaybackName(audio_.playbackDevices()[playbackIndex].name)) {
      return std::nullopt;
    }
    return std::pair{captureIndex, playbackIndex};
  }

  void renderRouteReadiness() {
    if (selectedSafeRoute().has_value()) {
      renderState(UiState::ready);
      return;
    }
    const bool hasCapture = std::ranges::any_of(audio_.captureDevices(), [](const auto& device) {
      return vvc::realtime::isSafeCaptureName(device.name);
    });
    const bool hasCable = std::ranges::any_of(audio_.playbackDevices(), [](const auto& device) {
      return vvc::realtime::isCablePlaybackName(device.name);
    });
    if (!hasCapture) {
      renderState(UiState::needs_setup,
                  L"CABLE Output "
                  L"以外のマイク入力が見つかりません。接続して「再読み込み」を押してください。");
    } else if (!hasCable) {
      renderState(
          UiState::needs_setup,
          L"CABLE Input が見つかりません。VB-CABLE を導入後、「再読み込み」を押してください。");
    } else {
      renderState(UiState::needs_setup, L"マイク入力と CABLE Input を選び、再度開始してください。");
    }
  }

  void renderState(const UiState state, const std::wstring_view customMessage = {}) {
    ui_state_ = state;
    const bool changing = state == UiState::starting;
    const bool running = state == UiState::running;
    const bool routeReady = selectedSafeRoute().has_value();
    EnableWindow(capture_combo_, !changing && !running);
    EnableWindow(playback_combo_, !changing && !running);
    EnableWindow(refresh_button_, !changing && !running);
    EnableWindow(start_button_,
                 running || (!changing && routeReady && state != UiState::recoverable_error));
    EnableWindow(bypass_checkbox_, running);
    EnableWindow(mute_checkbox_, running);
    SetWindowTextW(start_button_, running ? L"変換を停止" : L"変換を開始");

    std::wstring message(customMessage);
    if (message.empty()) {
      switch (state) {
      case UiState::needs_setup:
        message = L"準備が必要です。接続を確認し、「再読み込み」を押してください。";
        break;
      case UiState::ready:
        message = L"準備完了。「変換を開始」を押してください。";
        break;
      case UiState::starting:
        message = L"音声変換を開始中です…";
        break;
      case UiState::running:
        message = L"変換中です。停止するには「変換を停止」を押してください。";
        break;
      case UiState::stopped:
        message = L"停止しました。再開するには「変換を開始」を押してください。";
        break;
      case UiState::recoverable_error:
        message = L"安全に停止しました。接続を確認し、「再読み込み」を押してください。";
        break;
      }
    }
    SetWindowTextW(status_text_, message.c_str());
  }

  void handleCommand(const int id, const int notification) {
    if (id == kRefreshButton) {
      refreshDevices();
    } else if (id == kStartButton) {
      toggleAudio();
    } else if (id == kBypassCheckbox) {
      settings_.bypassed = SendMessageW(bypass_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
      audio_.setBypassed(settings_.bypassed);
    } else if (id == kMuteCheckbox) {
      audio_.setMuted(SendMessageW(mute_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    } else if ((id == kCaptureCombo || id == kPlaybackCombo) && notification == CBN_SELCHANGE) {
      persistSettings();
      renderRouteReadiness();
    }
  }

  void toggleAudio() {
    if (audio_.isRunning()) {
      audio_.stop();
      renderState(UiState::stopped);
      return;
    }

    const auto route = selectedSafeRoute();
    if (!route.has_value()) {
      renderRouteReadiness();
      return;
    }
    renderState(UiState::starting);
    audio_.setVoiceSettings(vvc::realtime::kNaturalVoiceSettings);
    audio_.setBypassed(settings_.bypassed);
    audio_.setMuted(SendMessageW(mute_checkbox_, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const auto result = audio_.start(route->first, route->second);
    if (result.succeeded) {
      persistSettings();
      renderState(UiState::running, result.message);
    } else {
      renderState(UiState::recoverable_error, result.message);
    }
  }

  void updateMetersAndLifecycle() {
    const auto service = audio_.service();
    if (service.state == vvc::realtime::AudioLifecycleState::recoverable_error &&
        ui_state_ != UiState::recoverable_error) {
      renderState(UiState::recoverable_error, recoveryMessage(service.failure));
    }

    const auto stats = audio_.takeStats();
    SendMessageW(input_meter_, PBM_SETPOS, meterValue(stats.input_peak), 0);
    SendMessageW(output_meter_, PBM_SETPOS, meterValue(stats.output_peak), 0);
    std::wstring health = L"DSP " + std::to_wstring(stats.algorithm_latency_ms) +
                          L" ms / queue in " + std::to_wstring(stats.input_queue_frames) + L"/" +
                          std::to_wstring(stats.max_input_queue_frames) + L" out " +
                          std::to_wstring(stats.output_queue_frames) + L"/" +
                          std::to_wstring(stats.max_output_queue_frames) + L" backlog " +
                          std::to_wstring(stats.avoidable_backlog_frames) + L"/" +
                          std::to_wstring(stats.max_avoidable_backlog_frames) + L"\r\nresync " +
                          std::to_wstring(stats.resynchronizations) + L" / underrun " +
                          std::to_wstring(stats.output_underruns) + L" / overrun " +
                          std::to_wstring(stats.input_overruns) + L" / deadline " +
                          std::to_wstring(stats.deadline_misses) + L" / fatal " +
                          std::to_wstring(stats.fatal_dsp_failures);
    SetWindowTextW(health_text_, health.c_str());
  }

  [[nodiscard]] static std::wstring
  recoveryMessage(const vvc::realtime::AudioFailureReason reason) {
    switch (reason) {
    case vvc::realtime::AudioFailureReason::dsp_failure:
      return L"音声処理エラーのため安全に停止しました。「再読み込み」後に再開してください。";
    case vvc::realtime::AudioFailureReason::device_open_failed:
    case vvc::realtime::AudioFailureReason::device_start_failed:
      return L"音声デバイスを開始できないため安全に停止しました。接続を確認して再読み込みしてくださ"
             L"い。";
    case vvc::realtime::AudioFailureReason::device_stopped:
    case vvc::realtime::AudioFailureReason::device_rerouted:
    case vvc::realtime::AudioFailureReason::interruption_began:
      return L"音声デバイスが切断または変更されたため安全に停止しました。再接続して「再読み込み」を"
             L"押してください。";
    case vvc::realtime::AudioFailureReason::none:
      return L"安全に停止しました。「再読み込み」後に再開してください。";
    }
    return L"安全に停止しました。「再読み込み」後に再開してください。";
  }

  void persistSettings() {
    const LRESULT capture = SendMessageW(capture_combo_, CB_GETCURSEL, 0, 0);
    const LRESULT playback = SendMessageW(playback_combo_, CB_GETCURSEL, 0, 0);
    if (capture != CB_ERR && capture >= 0 &&
        static_cast<std::size_t>(capture) < audio_.captureDevices().size() &&
        vvc::realtime::isSafeCaptureName(
            audio_.captureDevices()[static_cast<std::size_t>(capture)].name)) {
      settings_.capture_device = audio_.captureDevices()[static_cast<std::size_t>(capture)].name;
    }
    if (playback != CB_ERR && playback >= 0 &&
        static_cast<std::size_t>(playback) < audio_.playbackDevices().size() &&
        vvc::realtime::isCablePlaybackName(
            audio_.playbackDevices()[static_cast<std::size_t>(playback)].name)) {
      settings_.playback_device = audio_.playbackDevices()[static_cast<std::size_t>(playback)].name;
    }
    vvc::windows::saveAppSettings(settings_);
  }

  [[nodiscard]] bool verifySmokeInterface() const {
    wchar_t title[128]{};
    GetWindowTextW(window_, title, static_cast<int>(std::size(title)));
    const int statusLength = GetWindowTextLengthW(status_text_);
    std::wstring status(static_cast<std::size_t>(std::max(statusLength, 0)) + 1U, L'\0');
    GetWindowTextW(status_text_, status.data(), static_cast<int>(status.size()));
    const HWND voiceSummary = GetDlgItem(window_, kVoiceSummary);
    const int voiceLength = voiceSummary != nullptr ? GetWindowTextLengthW(voiceSummary) : 0;
    std::wstring voiceText(static_cast<std::size_t>(std::max(voiceLength, 0)) + 1U, L'\0');
    if (voiceSummary != nullptr) {
      GetWindowTextW(voiceSummary, voiceText.data(), static_cast<int>(voiceText.size()));
    }
    const bool routeReady = selectedSafeRoute().has_value();
    if (std::wstring_view(title) != kWindowTitle || audio_.isRunning() ||
        capture_combo_ == nullptr || playback_combo_ == nullptr || refresh_button_ == nullptr ||
        start_button_ == nullptr || bypass_checkbox_ == nullptr || mute_checkbox_ == nullptr ||
        input_meter_ == nullptr || output_meter_ == nullptr || status_text_ == nullptr ||
        health_text_ == nullptr || voiceSummary == nullptr ||
        voiceText.find(L"Natural") == std::wstring::npos ||
        GetDlgItem(window_, kRemovedCuteButton) != nullptr ||
        GetDlgItem(window_, kRemovedClearButton) != nullptr ||
        GetDlgItem(window_, kRemovedPitchSlider) != nullptr ||
        GetDlgItem(window_, kRemovedFormantSlider) != nullptr || !IsWindowEnabled(capture_combo_) ||
        !IsWindowEnabled(playback_combo_) || !IsWindowEnabled(refresh_button_) ||
        IsWindowEnabled(bypass_checkbox_) || IsWindowEnabled(mute_checkbox_) || statusLength <= 0 ||
        (IsWindowEnabled(start_button_) != static_cast<BOOL>(routeReady)) ||
        (routeReady && status.find(L"開始") == std::wstring::npos) ||
        (!routeReady && status.find(L"再読み込み") == std::wstring::npos)) {
      return false;
    }
    return true;
  }

  HINSTANCE instance_ = nullptr;
  bool smoke_test_ = false;
  bool controls_valid_ = true;
  int smoke_exit_code_ = 0;
  UiState ui_state_ = UiState::needs_setup;
  HWND window_ = nullptr;
  HWND capture_combo_ = nullptr;
  HWND playback_combo_ = nullptr;
  HWND refresh_button_ = nullptr;
  HWND start_button_ = nullptr;
  HWND bypass_checkbox_ = nullptr;
  HWND mute_checkbox_ = nullptr;
  HWND input_meter_ = nullptr;
  HWND output_meter_ = nullptr;
  HWND status_text_ = nullptr;
  HWND health_text_ = nullptr;
  HFONT font_ = nullptr;
  HFONT title_font_ = nullptr;
  HBRUSH background_brush_ = nullptr;
  vvc::windows::AppSettings settings_;
  vvc::windows::AudioEngine audio_;
};

[[nodiscard]] int runApplication(const HINSTANCE instance, PWSTR commandLine) {
  const std::wstring_view arguments = commandLine != nullptr ? commandLine : L"";
  if (arguments.find(L"--self-test") != std::wstring_view::npos) {
    return vvc::windows::runExecutableSelfTest();
  }

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS};
  if (InitCommonControlsEx(&controls) == FALSE) {
    return 2;
  }

  const bool smokeTest = arguments.find(L"--ui-smoke-test") != std::wstring_view::npos;
  MainWindow mainWindow(instance, smokeTest);
  if (!mainWindow.create()) {
    if (!smokeTest) {
      MessageBoxW(nullptr, L"アプリを起動できません。", kWindowTitle, MB_ICONERROR);
    }
    return 1;
  }

  MSG message{};
  BOOL getMessageResult = 0;
  while ((getMessageResult = GetMessageW(&message, nullptr, 0U, 0U)) != 0) {
    if (getMessageResult == -1) {
      return 4;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  const int exitCode = static_cast<int>(message.wParam);
  if (exitCode == 4 && !smokeTest) {
    MessageBoxW(nullptr, L"予期しないエラーのためアプリを安全に終了しました。", kWindowTitle,
                MB_OK | MB_ICONERROR);
  }
  return exitCode;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
  const std::wstring_view arguments = commandLine != nullptr ? commandLine : L"";
  const bool unattendedTest = arguments.find(L"--self-test") != std::wstring_view::npos ||
                              arguments.find(L"--ui-smoke-test") != std::wstring_view::npos;
  try {
    return runApplication(instance, commandLine);
  } catch (...) {
    if (!unattendedTest) {
      MessageBoxW(nullptr, L"予期しないエラーのためアプリを安全に終了しました。", kWindowTitle,
                  MB_OK | MB_ICONERROR);
    }
    return 4;
  }
}
