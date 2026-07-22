#include "core/audio/WavFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void writeU16(std::ostream& stream, const std::uint16_t value) {
  stream.put(static_cast<char>(value & 0xFFU));
  stream.put(static_cast<char>((value >> 8U) & 0xFFU));
}

void writeU32(std::ostream& stream, const std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    stream.put(static_cast<char>((value >> shift) & 0xFFU));
  }
}

void writeExtensiblePcm(const std::filesystem::path& path, const std::vector<float>& samples,
                        const std::uint16_t bitsPerSample) {
  const std::uint16_t bytesPerSample = static_cast<std::uint16_t>(bitsPerSample / 8U);
  const auto dataSize =
      static_cast<std::uint32_t>(samples.size() * static_cast<std::size_t>(bytesPerSample));
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("fixture output could not be opened");
  }

  stream.write("RIFF", 4);
  writeU32(stream, 60U + dataSize);
  stream.write("WAVEfmt ", 8);
  writeU32(stream, 40U);
  writeU16(stream, 0xFFFEU);
  writeU16(stream, 1U);
  writeU32(stream, 48'000U);
  writeU32(stream, 48'000U * static_cast<std::uint32_t>(bytesPerSample));
  writeU16(stream, bytesPerSample);
  writeU16(stream, bitsPerSample);
  writeU16(stream, 22U);
  writeU16(stream, bitsPerSample);
  writeU32(stream, 0x00000004U);
  constexpr std::array<std::uint8_t, 16> pcmSubformat{0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                                                      0x10U, 0x00U, 0x80U, 0x00U, 0x00U, 0xAAU,
                                                      0x00U, 0x38U, 0x9BU, 0x71U};
  for (const std::uint8_t byte : pcmSubformat) {
    stream.put(static_cast<char>(byte));
  }
  stream.write("data", 4);
  writeU32(stream, dataSize);

  const double scale = bitsPerSample == 24U ? 8'388'607.0 : 2'147'483'647.0;
  for (const float original : samples) {
    const double clamped = std::clamp(static_cast<double>(original), -1.0, 1.0);
    const auto sample = static_cast<std::int32_t>(std::llround(clamped * scale));
    const auto raw = static_cast<std::uint32_t>(sample);
    for (std::uint16_t byte = 0U; byte < bytesPerSample; ++byte) {
      const unsigned int shift = static_cast<unsigned int>(byte) * 8U;
      stream.put(static_cast<char>((raw >> shift) & 0xFFU));
    }
  }
  if (!stream) {
    throw std::runtime_error("fixture output could not be written");
  }
}

} // namespace

int main(const int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    return 2;
  }
  const std::filesystem::path output(argv[1]);
  std::filesystem::create_directories(output.parent_path());
  std::vector<float> samples(24'000U);
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const double time = static_cast<double>(index) / 48'000.0;
    samples[index] = static_cast<float>(0.3 * std::sin(2.0 * std::numbers::pi * 200.0 * time) +
                                        0.1 * std::sin(2.0 * std::numbers::pi * 600.0 * time));
  }
  if (argc == 2) {
    vvc::audio::writeMonoPcm16Wav(output, {48'000U, std::move(samples)});
  } else {
    const std::string_view format(argv[2]);
    if (format == "pcm24") {
      writeExtensiblePcm(output, samples, 24U);
    } else if (format == "pcm32") {
      writeExtensiblePcm(output, samples, 32U);
    } else {
      return 2;
    }
  }
  return 0;
}
