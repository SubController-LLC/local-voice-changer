#include "realtime/DeviceRouting.h"

#include <array>

#include <catch2/catch_test_macros.hpp>

using vvc::realtime::RoutingDevice;

TEST_CASE("capture routing rejects cable output and prefers a safe saved device") {
  const std::array devices{RoutingDevice{L"CABLE Output (VB-Audio Virtual Cable)", true},
                           RoutingDevice{L"USB Microphone", false},
                           RoutingDevice{L"Headset Microphone", true}};
  REQUIRE_FALSE(vvc::realtime::isSafeCaptureName(devices[0].name));
  REQUIRE(vvc::realtime::chooseSafeCapture(devices, L"USB Microphone") == 1U);
  REQUIRE(vvc::realtime::chooseSafeCapture(devices, L"missing") == 2U);
}

TEST_CASE("capture routing reports no route when only cable output exists") {
  const std::array devices{RoutingDevice{L"cable output", true}};
  REQUIRE_FALSE(vvc::realtime::chooseSafeCapture(devices).has_value());
}

TEST_CASE("playback routing requires an actual cable input endpoint") {
  const std::array devices{RoutingDevice{L"Speakers", true},
                           RoutingDevice{L"CABLE Input (VB-Audio Virtual Cable)", false}};
  REQUIRE(vvc::realtime::chooseCablePlayback(devices, L"Speakers") == 1U);
  REQUIRE(vvc::realtime::isCablePlaybackName(devices[1].name));

  const std::array noCable{RoutingDevice{L"Speakers", true}, RoutingDevice{L"HDMI", false}};
  REQUIRE_FALSE(vvc::realtime::chooseCablePlayback(noCable).has_value());
}
