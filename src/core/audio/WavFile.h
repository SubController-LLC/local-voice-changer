#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace vvc::audio {

inline constexpr std::uint32_t kRequiredSampleRate = 48'000U;

struct WavData {
  std::uint32_t sample_rate = kRequiredSampleRate;
  std::vector<float> samples;
};

[[nodiscard]] WavData readMonoWav48k(const std::filesystem::path& path);
void writeMonoPcm16Wav(const std::filesystem::path& path, const WavData& audio);

} // namespace vvc::audio
