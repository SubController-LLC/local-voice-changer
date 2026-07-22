#include "core/audio/WavFile.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

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

void writeMinimalWav(const std::filesystem::path& path, const std::uint32_t sampleRate,
                     const std::uint16_t channels) {
  std::ofstream stream(path, std::ios::binary);
  const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * 2U);
  const std::uint32_t dataSize = static_cast<std::uint32_t>(blockAlign);
  stream.write("RIFF", 4);
  writeU32(stream, 36U + dataSize);
  stream.write("WAVEfmt ", 8);
  writeU32(stream, 16U);
  writeU16(stream, 1U);
  writeU16(stream, channels);
  writeU32(stream, sampleRate);
  writeU32(stream, sampleRate * static_cast<std::uint32_t>(blockAlign));
  writeU16(stream, blockAlign);
  writeU16(stream, 16U);
  stream.write("data", 4);
  writeU32(stream, dataSize);
  for (std::uint32_t index = 0U; index < dataSize; ++index) {
    stream.put('\0');
  }
}

void writeExtensiblePcm(const std::filesystem::path& path, const std::uint16_t bitsPerSample,
                        const std::uint16_t validBits, const std::uint32_t channelMask,
                        const std::uint32_t subformatCode) {
  const std::uint16_t bytesPerSample = static_cast<std::uint16_t>(bitsPerSample / 8U);
  constexpr std::array<std::int32_t, 3> samples{-1, 0, 1};
  const auto dataSize =
      static_cast<std::uint32_t>(samples.size() * static_cast<std::size_t>(bytesPerSample));
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
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
  writeU16(stream, validBits);
  writeU32(stream, channelMask);
  writeU32(stream, subformatCode);
  writeU16(stream, 0U);
  writeU16(stream, 0x0010U);
  constexpr std::array<std::uint8_t, 8> guidSuffix{0x80U, 0x00U, 0x00U, 0xAAU,
                                                   0x00U, 0x38U, 0x9BU, 0x71U};
  for (const std::uint8_t byte : guidSuffix) {
    stream.put(static_cast<char>(byte));
  }
  stream.write("data", 4);
  writeU32(stream, dataSize);
  for (const std::int32_t sample : samples) {
    const std::uint32_t raw = static_cast<std::uint32_t>(sample);
    for (std::uint16_t byte = 0U; byte < bytesPerSample; ++byte) {
      const unsigned int shift = static_cast<unsigned int>(byte) * 8U;
      stream.put(static_cast<char>((raw >> shift) & 0xFFU));
    }
  }
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() / ("vvc-tests-" + std::to_string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace

TEST_CASE("PCM16 WAV round-trips as 48 kHz mono") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "roundtrip.wav";
  const vvc::audio::WavData original{48'000U, {-1.0F, -0.5F, 0.0F, 0.5F, 1.0F}};
  vvc::audio::writeMonoPcm16Wav(path, original);
  const auto decoded = vvc::audio::readMonoWav48k(path);

  REQUIRE(decoded.sample_rate == 48'000U);
  REQUIRE(decoded.samples.size() == original.samples.size());
  for (std::size_t index = 0; index < decoded.samples.size(); ++index) {
    CHECK(decoded.samples[index] == Catch::Approx(original.samples[index]).margin(0.00004F));
  }
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE PCM24 is decoded") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "extensible-pcm24.wav";
  writeExtensiblePcm(path, 24U, 24U, 0x00000004U, 1U);

  const auto decoded = vvc::audio::readMonoWav48k(path);
  REQUIRE(decoded.samples.size() == 3U);
  CHECK(decoded.samples[0] == Catch::Approx(-1.0F / 8'388'608.0F));
  CHECK(decoded.samples[1] == Catch::Approx(0.0F));
  CHECK(decoded.samples[2] == Catch::Approx(1.0F / 8'388'608.0F));
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE PCM32 is decoded") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "extensible-pcm32.wav";
  writeExtensiblePcm(path, 32U, 32U, 0x00000004U, 1U);

  const auto decoded = vvc::audio::readMonoWav48k(path);
  REQUIRE(decoded.samples.size() == 3U);
  CHECK(decoded.samples[0] == Catch::Approx(-1.0F / 2'147'483'648.0F));
  CHECK(decoded.samples[1] == Catch::Approx(0.0F));
  CHECK(decoded.samples[2] == Catch::Approx(1.0F / 2'147'483'648.0F));
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE rejects unsupported metadata") {
  TemporaryDirectory temporary;

  const auto validBitsPath = temporary.path() / "invalid-valid-bits.wav";
  writeExtensiblePcm(validBitsPath, 24U, 20U, 0x00000004U, 1U);
  REQUIRE_THROWS_WITH(vvc::audio::readMonoWav48k(validBitsPath),
                      "コンテナ幅と有効ビット数が異なるWAVには対応していません。");

  const auto maskPath = temporary.path() / "invalid-mask.wav";
  writeExtensiblePcm(maskPath, 24U, 24U, 0x00000003U, 1U);
  REQUIRE_THROWS_WITH(vvc::audio::readMonoWav48k(maskPath),
                      "WAVE_FORMAT_EXTENSIBLEのチャンネルマスクが不正です。");

  const auto subformatPath = temporary.path() / "invalid-subformat.wav";
  writeExtensiblePcm(subformatPath, 24U, 24U, 0x00000004U, 2U);
  REQUIRE_THROWS_WITH(
      vvc::audio::readMonoWav48k(subformatPath),
      "WAVE_FORMAT_EXTENSIBLEはPCMまたは32-bit floatのSubFormatのみ対応しています。");
}

TEST_CASE("corrupt input fails with a Japanese reason") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "corrupt.wav";
  {
    std::ofstream stream(path, std::ios::binary);
    stream << "not a wave";
  }
  REQUIRE_THROWS_WITH(vvc::audio::readMonoWav48k(path), "WAVファイルが短すぎます。");
}

TEST_CASE("writer rejects sample rates other than 48 kHz") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "wrong-rate.wav";
  REQUIRE_THROWS_WITH(vvc::audio::writeMonoPcm16Wav(path, {44'100U, {0.0F}}),
                      "出力WAVのサンプルレートは48 kHzである必要があります。");
}

TEST_CASE("reader rejects sample rates other than 48 kHz") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "wrong-input-rate.wav";
  writeMinimalWav(path, 44'100U, 1U);
  REQUIRE_THROWS_WITH(vvc::audio::readMonoWav48k(path), "入力は48 kHz WAVにしてください。");
}

TEST_CASE("reader rejects stereo input") {
  TemporaryDirectory temporary;
  const auto path = temporary.path() / "stereo.wav";
  writeMinimalWav(path, 48'000U, 2U);
  REQUIRE_THROWS_WITH(vvc::audio::readMonoWav48k(path), "入力はmono WAVにしてください。");
}
