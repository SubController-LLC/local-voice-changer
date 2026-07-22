#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vvc::realtime {

struct RoutingDevice {
  std::wstring name;
  bool is_default = false;
};

[[nodiscard]] bool isSafeCaptureName(std::wstring_view name);
[[nodiscard]] bool isCablePlaybackName(std::wstring_view name);
[[nodiscard]] std::optional<std::size_t> chooseSafeCapture(std::span<const RoutingDevice> devices,
                                                           std::wstring_view savedName = {});
[[nodiscard]] std::optional<std::size_t> chooseCablePlayback(std::span<const RoutingDevice> devices,
                                                             std::wstring_view savedName = {});

} // namespace vvc::realtime
