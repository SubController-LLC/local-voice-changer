#include "realtime/DeviceRouting.h"

#include <algorithm>
#include <cwctype>

namespace vvc::realtime {
namespace {

[[nodiscard]] bool containsInsensitive(std::wstring_view text, std::wstring_view needle) {
  if (needle.empty() || text.size() < needle.size()) {
    return false;
  }
  return std::ranges::search(text, needle, [](const wchar_t left, const wchar_t right) {
           return std::towlower(static_cast<wint_t>(left)) ==
                  std::towlower(static_cast<wint_t>(right));
         }).begin() != text.end();
}

} // namespace

bool isSafeCaptureName(const std::wstring_view name) {
  return !name.empty() && !containsInsensitive(name, L"CABLE Output");
}

bool isCablePlaybackName(const std::wstring_view name) {
  return containsInsensitive(name, L"CABLE Input");
}

std::optional<std::size_t> chooseSafeCapture(const std::span<const RoutingDevice> devices,
                                             const std::wstring_view savedName) {
  std::optional<std::size_t> firstSafe;
  std::optional<std::size_t> defaultSafe;
  for (std::size_t index = 0U; index < devices.size(); ++index) {
    const auto& device = devices[index];
    if (!isSafeCaptureName(device.name)) {
      continue;
    }
    if (!firstSafe.has_value()) {
      firstSafe = index;
    }
    if (device.is_default && !defaultSafe.has_value()) {
      defaultSafe = index;
    }
    if (!savedName.empty() && device.name == savedName) {
      return index;
    }
  }
  return defaultSafe.has_value() ? defaultSafe : firstSafe;
}

std::optional<std::size_t> chooseCablePlayback(const std::span<const RoutingDevice> devices,
                                               const std::wstring_view savedName) {
  std::optional<std::size_t> firstCable;
  for (std::size_t index = 0U; index < devices.size(); ++index) {
    const auto& device = devices[index];
    if (!isCablePlaybackName(device.name)) {
      continue;
    }
    if (!firstCable.has_value()) {
      firstCable = index;
    }
    if (!savedName.empty() && device.name == savedName) {
      return index;
    }
  }
  return firstCable;
}

} // namespace vvc::realtime
