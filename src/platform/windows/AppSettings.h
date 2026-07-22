#pragma once

#include <string>

namespace vvc::windows {

struct AppSettings {
  std::wstring capture_device;
  std::wstring playback_device;
  bool bypassed = false;
};

[[nodiscard]] AppSettings loadAppSettings();
void saveAppSettings(const AppSettings& settings);

} // namespace vvc::windows
