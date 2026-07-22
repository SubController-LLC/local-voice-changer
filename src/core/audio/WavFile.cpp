#include "core/audio/WavFile.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace vvc::audio {
namespace {

constexpr std::uint16_t kPcmFormat = 0x0001U;
constexpr std::uint16_t kFloatFormat = 0x0003U;
constexpr std::uint16_t kExtensibleFormat = 0xFFFEU;
constexpr std::array<std::uint8_t, 16> kPcmSubformat{0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                                                     0x10U, 0x00U, 0x80U, 0x00U, 0x00U, 0xAAU,
                                                     0x00U, 0x38U, 0x9BU, 0x71U};
constexpr std::array<std::uint8_t, 16> kFloatSubformat{0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                                                       0x10U, 0x00U, 0x80U, 0x00U, 0x00U, 0xAAU,
                                                       0x00U, 0x38U, 0x9BU, 0x71U};

[[nodiscard]] std::uint16_t readU16(const std::span<const std::byte> bytes,
                                    const std::size_t offset) {
  if (offset + 2U > bytes.size()) {
    throw std::runtime_error("WAVヘッダーが途中で切れています。");
  }
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
         static_cast<std::uint16_t>(
             static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U);
}

[[nodiscard]] std::uint32_t readU32(const std::span<const std::byte> bytes,
                                    const std::size_t offset) {
  if (offset + 4U > bytes.size()) {
    throw std::runtime_error("WAVヘッダーが途中で切れています。");
  }
  std::uint32_t value = 0U;
  for (std::size_t index = 0; index < 4U; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
             << static_cast<unsigned int>(index * 8U);
  }
  return value;
}

[[nodiscard]] bool matches(const std::span<const std::byte> bytes, const std::size_t offset,
                           const std::string_view expected) {
  if (offset + expected.size() > bytes.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (std::to_integer<char>(bytes[offset + index]) != expected[index]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool matches(const std::span<const std::byte> bytes, const std::size_t offset,
                           const std::array<std::uint8_t, 16>& expected) {
  if (offset + expected.size() > bytes.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (std::to_integer<std::uint8_t>(bytes[offset + index]) != expected[index]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::uint16_t resolveFormat(const std::span<const std::byte> bytes,
                                          const std::size_t payloadOffset,
                                          const std::uint32_t chunkSize, const std::uint16_t format,
                                          const std::uint16_t channels,
                                          const std::uint16_t bitsPerSample) {
  if (format != kExtensibleFormat) {
    return format;
  }
  if (chunkSize < 40U) {
    throw std::runtime_error("WAVE_FORMAT_EXTENSIBLEのfmtチャンクが短すぎます。");
  }

  const std::uint16_t extensionSize = readU16(bytes, payloadOffset + 16U);
  if (extensionSize < 22U || static_cast<std::uint32_t>(extensionSize) + 18U > chunkSize) {
    throw std::runtime_error("WAVE_FORMAT_EXTENSIBLEの拡張サイズが不正です。");
  }

  const std::uint16_t validBits = readU16(bytes, payloadOffset + 18U);
  if (validBits == 0U || validBits != bitsPerSample) {
    throw std::runtime_error("コンテナ幅と有効ビット数が異なるWAVには対応していません。");
  }

  const std::uint32_t channelMask = readU32(bytes, payloadOffset + 20U);
  if (channelMask != 0U && std::popcount(channelMask) != static_cast<int>(channels)) {
    throw std::runtime_error("WAVE_FORMAT_EXTENSIBLEのチャンネルマスクが不正です。");
  }

  const std::size_t subformatOffset = payloadOffset + 24U;
  if (matches(bytes, subformatOffset, kPcmSubformat)) {
    return kPcmFormat;
  }
  if (matches(bytes, subformatOffset, kFloatSubformat)) {
    return kFloatFormat;
  }
  throw std::runtime_error(
      "WAVE_FORMAT_EXTENSIBLEはPCMまたは32-bit floatのSubFormatのみ対応しています。");
}

void writeU16(std::ostream& stream, const std::uint16_t value) {
  const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU),
                                  static_cast<char>((value >> 8U) & 0xFFU)};
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void writeU32(std::ostream& stream, const std::uint32_t value) {
  const std::array<char, 4> bytes{
      static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
      static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>((value >> 24U) & 0xFFU)};
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] float decodePcm(const std::span<const std::byte> bytes, const std::size_t offset,
                              const std::uint16_t bitsPerSample) {
  if (bitsPerSample == 16U) {
    const auto raw = static_cast<std::int16_t>(readU16(bytes, offset));
    return static_cast<float>(raw) / 32768.0F;
  }
  if (bitsPerSample == 24U) {
    if (offset + 3U > bytes.size()) {
      throw std::runtime_error("WAV音声データが途中で切れています。");
    }
    std::int32_t raw =
        static_cast<std::int32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
        (static_cast<std::int32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U) |
        (static_cast<std::int32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 16U);
    if ((raw & 0x00800000) != 0) {
      raw |= static_cast<std::int32_t>(0xFF000000U);
    }
    return static_cast<float>(raw) / 8388608.0F;
  }
  if (bitsPerSample == 32U) {
    const auto raw = static_cast<std::int32_t>(readU32(bytes, offset));
    return static_cast<float>(static_cast<double>(raw) / 2147483648.0);
  }
  throw std::runtime_error("対応していないPCMビット深度です（16/24/32 bitのみ対応）。");
}

[[nodiscard]] float decodeFloat32(const std::span<const std::byte> bytes,
                                  const std::size_t offset) {
  const std::uint32_t raw = readU32(bytes, offset);
  return std::bit_cast<float>(raw);
}

} // namespace

WavData readMonoWav48k(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) {
    throw std::runtime_error("入力WAVを開けません: " + path.string());
  }

  const std::streamsize fileSize = stream.tellg();
  if (fileSize < 12) {
    throw std::runtime_error("WAVファイルが短すぎます。");
  }
  stream.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(fileSize));
  stream.read(reinterpret_cast<char*>(bytes.data()), fileSize);
  if (!stream) {
    throw std::runtime_error("WAVファイルを最後まで読み込めません。");
  }

  const std::span<const std::byte> view(bytes);
  if (!matches(view, 0U, "RIFF") || !matches(view, 8U, "WAVE")) {
    throw std::runtime_error("RIFF/WAVE形式ではありません。");
  }

  bool hasFormat = false;
  bool hasData = false;
  std::uint16_t format = 0U;
  std::uint16_t channels = 0U;
  std::uint32_t sampleRate = 0U;
  std::uint16_t blockAlign = 0U;
  std::uint16_t bitsPerSample = 0U;
  std::size_t dataOffset = 0U;
  std::size_t dataSize = 0U;

  std::size_t cursor = 12U;
  while (cursor + 8U <= view.size()) {
    const std::uint32_t chunkSize = readU32(view, cursor + 4U);
    const std::size_t payloadOffset = cursor + 8U;
    if (payloadOffset > view.size() ||
        static_cast<std::size_t>(chunkSize) > view.size() - payloadOffset) {
      throw std::runtime_error("WAVチャンクサイズがファイル範囲を超えています。");
    }

    if (matches(view, cursor, "fmt ")) {
      if (chunkSize < 16U) {
        throw std::runtime_error("WAVのfmtチャンクが不正です。");
      }
      format = readU16(view, payloadOffset);
      channels = readU16(view, payloadOffset + 2U);
      sampleRate = readU32(view, payloadOffset + 4U);
      blockAlign = readU16(view, payloadOffset + 12U);
      bitsPerSample = readU16(view, payloadOffset + 14U);
      format = resolveFormat(view, payloadOffset, chunkSize, format, channels, bitsPerSample);
      hasFormat = true;
    } else if (matches(view, cursor, "data")) {
      dataOffset = payloadOffset;
      dataSize = static_cast<std::size_t>(chunkSize);
      hasData = true;
    }

    cursor = payloadOffset + static_cast<std::size_t>(chunkSize) + (chunkSize % 2U);
  }

  if (!hasFormat || !hasData) {
    throw std::runtime_error("WAVにfmtまたはdataチャンクがありません。");
  }
  if (channels != 1U) {
    throw std::runtime_error("入力はmono WAVにしてください。");
  }
  if (sampleRate != kRequiredSampleRate) {
    throw std::runtime_error("入力は48 kHz WAVにしてください。");
  }
  if (format != kPcmFormat && format != kFloatFormat) {
    throw std::runtime_error("対応していないWAV形式です（PCMまたは32-bit floatのみ対応）。");
  }
  if (format == kFloatFormat && bitsPerSample != 32U) {
    throw std::runtime_error("float WAVは32 bitのみ対応しています。");
  }
  if (format == kPcmFormat && bitsPerSample != 16U && bitsPerSample != 24U &&
      bitsPerSample != 32U) {
    throw std::runtime_error("対応していないPCMビット深度です（16/24/32 bitのみ対応）。");
  }
  const std::size_t bytesPerSample = static_cast<std::size_t>(bitsPerSample / 8U);
  if (bitsPerSample % 8U != 0U || bytesPerSample == 0U || blockAlign != bytesPerSample ||
      dataSize % blockAlign != 0U) {
    throw std::runtime_error("WAVのサンプル配置が不正です。");
  }

  WavData result{sampleRate, {}};
  const std::size_t sampleCount = dataSize / bytesPerSample;
  result.samples.reserve(sampleCount);
  for (std::size_t index = 0; index < sampleCount; ++index) {
    const std::size_t offset = dataOffset + index * bytesPerSample;
    result.samples.push_back(format == kFloatFormat ? decodeFloat32(view, offset)
                                                    : decodePcm(view, offset, bitsPerSample));
  }
  if (result.samples.empty()) {
    throw std::runtime_error("WAVに音声サンプルがありません。");
  }
  return result;
}

void writeMonoPcm16Wav(const std::filesystem::path& path, const WavData& audio) {
  if (audio.sample_rate != kRequiredSampleRate) {
    throw std::runtime_error("出力WAVのサンプルレートは48 kHzである必要があります。");
  }
  if (audio.samples.size() >
      (static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) - 36U) / 2U) {
    throw std::runtime_error("出力WAVが大きすぎます。");
  }

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("出力WAVを作成できません: " + path.string());
  }

  const auto dataBytes = static_cast<std::uint32_t>(audio.samples.size() * 2U);
  stream.write("RIFF", 4);
  writeU32(stream, 36U + dataBytes);
  stream.write("WAVEfmt ", 8);
  writeU32(stream, 16U);
  writeU16(stream, 1U);
  writeU16(stream, 1U);
  writeU32(stream, audio.sample_rate);
  writeU32(stream, audio.sample_rate * 2U);
  writeU16(stream, 2U);
  writeU16(stream, 16U);
  stream.write("data", 4);
  writeU32(stream, dataBytes);

  for (const float original : audio.samples) {
    const float finite = std::isfinite(original) ? original : 0.0F;
    const float clamped = std::clamp(finite, -1.0F, 1.0F);
    const auto sample = static_cast<std::int16_t>(
        std::lround(static_cast<double>(clamped) * (clamped < 0.0F ? 32768.0 : 32767.0)));
    writeU16(stream, static_cast<std::uint16_t>(sample));
  }
  if (!stream) {
    throw std::runtime_error("出力WAVの書き込みに失敗しました。");
  }
}

} // namespace vvc::audio
