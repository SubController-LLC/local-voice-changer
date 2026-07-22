#include "platform/windows/AppSettings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

#include <nlohmann/json.hpp>

#include <shlobj.h>
#include <windows.h>

namespace vvc::windows {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] fs::path settingsPath(const std::wstring_view applicationFolder) {
  PWSTR localAppData = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData))) {
    return {};
  }
  const fs::path base(localAppData);
  CoTaskMemFree(localAppData);
  return base / L"SubController" / applicationFolder / L"settings.json";
}

[[nodiscard]] std::string wideToUtf8(const std::wstring_view input) {
  if (input.empty()) {
    return {};
  }
  const int count = WideCharToMultiByte(CP_UTF8, 0U, input.data(), static_cast<int>(input.size()),
                                        nullptr, 0, nullptr, nullptr);
  std::string output(static_cast<std::size_t>(std::max(count, 0)), '\0');
  if (count > 0) {
    WideCharToMultiByte(CP_UTF8, 0U, input.data(), static_cast<int>(input.size()), output.data(),
                        count, nullptr, nullptr);
  }
  return output;
}

[[nodiscard]] std::wstring utf8ToWide(const std::string_view input) {
  if (input.empty()) {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                        static_cast<int>(input.size()), nullptr, 0);
  if (count <= 0) {
    return {};
  }
  std::wstring output(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
                      output.data(), count);
  return output;
}

} // namespace

AppSettings loadAppSettings() {
  AppSettings settings;
  try {
    const fs::path path = settingsPath(L"LocalVoiceChanger");
    if (path.empty()) {
      return settings;
    }
    std::ifstream stream(path);
    if (!stream) {
      return settings;
    }
    const nlohmann::json json = nlohmann::json::parse(stream, nullptr, true, true);
    settings.capture_device = utf8ToWide(json.value("capture_device", std::string{}));
    settings.playback_device = utf8ToWide(json.value("playback_device", std::string{}));
    // Legacy preset and semitone fields are intentionally accepted but ignored. The public beta
    // has one validated Natural voice.
    settings.bypassed = json.value("bypassed", false);
  } catch (...) {
    return AppSettings{};
  }
  return settings;
}

void saveAppSettings(const AppSettings& settings) {
  try {
    const fs::path path = settingsPath(L"LocalVoiceChanger");
    if (path.empty()) {
      return;
    }
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
      return;
    }
    const nlohmann::ordered_json json{{"schema_version", 2},
                                      {"capture_device", wideToUtf8(settings.capture_device)},
                                      {"playback_device", wideToUtf8(settings.playback_device)},
                                      {"bypassed", settings.bypassed}};
    std::ofstream stream(path, std::ios::trunc);
    stream << json.dump(2) << '\n';
  } catch (...) {
    // Settings are a convenience; audio must continue even if persistence is unavailable.
  }
}

} // namespace vvc::windows
