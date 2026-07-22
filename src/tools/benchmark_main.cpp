#include "core/audio/SignalMetrics.h"
#include "core/audio/WavFile.h"
#include "core/dsp/VoiceProcessor.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef VVC_VERSION
#define VVC_VERSION "development"
#endif

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

struct Arguments {
  fs::path input;
  fs::path output_dir;
  vvc::dsp::VoiceSettings settings;
  bool show_help = false;
  bool show_version = false;
};

[[nodiscard]] std::string pathToUtf8(const fs::path& path) {
  const std::u8string utf8 = path.u8string();
  return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

[[nodiscard]] float parseFloat(const std::string_view text, const std::string_view option) {
  std::istringstream stream(std::string{text});
  stream.imbue(std::locale::classic());
  stream >> std::noskipws;
  float value = 0.0F;
  stream >> value;
  if (!stream || !stream.eof()) {
    throw std::runtime_error(std::string(option) +
                             " の値が数値ではありません: " + std::string(text));
  }
  return value;
}

[[nodiscard]] Arguments parseArguments(const std::vector<fs::path>& commandLine) {
  Arguments arguments;
  for (std::size_t index = 1U; index < commandLine.size(); ++index) {
    const std::string option = pathToUtf8(commandLine[index]);
    if (option == "--help" || option == "-h") {
      arguments.show_help = true;
    } else if (option == "--version") {
      arguments.show_version = true;
    } else if (option == "--input" || option == "--output-dir" || option == "--pitch" ||
               option == "--formant") {
      if (index + 1U >= commandLine.size()) {
        throw std::runtime_error(option + " の値がありません。");
      }
      const fs::path& value = commandLine[++index];
      if (option == "--input") {
        arguments.input = value;
      } else if (option == "--output-dir") {
        arguments.output_dir = value;
      } else if (option == "--pitch") {
        arguments.settings.pitch_semitones = parseFloat(pathToUtf8(value), option);
      } else {
        arguments.settings.formant_semitones = parseFloat(pathToUtf8(value), option);
      }
    } else {
      throw std::runtime_error("不明な引数です: " + option);
    }
  }

  if (!arguments.show_help && !arguments.show_version) {
    if (arguments.input.empty()) {
      throw std::runtime_error("--input で権利を持つローカルWAVを指定してください。");
    }
    if (arguments.output_dir.empty()) {
      throw std::runtime_error("--output-dir を指定してください。");
    }
    if (!vvc::dsp::VoiceProcessor::isValid(arguments.settings)) {
      throw std::runtime_error("Pitchは0〜+7、Formantは-2〜+6 semitonesで指定してください。");
    }
  }
  return arguments;
}

[[nodiscard]] fs::path normalizedPath(const fs::path& path) {
  std::error_code error;
  fs::path normalized = fs::weakly_canonical(path, error);
  if (!error) {
    return normalized.lexically_normal();
  }
  error.clear();
  normalized = fs::absolute(path, error);
  if (error) {
    throw std::runtime_error("入出力パスを正規化できません: " + pathToUtf8(path));
  }
  return normalized.lexically_normal();
}

void ensureInputDoesNotCollide(const fs::path& input, const std::array<fs::path, 4>& outputPaths) {
  const fs::path normalizedInput = normalizedPath(input);
  for (const fs::path& output : outputPaths) {
    std::error_code error;
    const bool outputExists = fs::exists(output, error);
    if (error) {
      throw std::runtime_error("出力先を確認できません: " + pathToUtf8(output));
    }

    bool collides = false;
    if (outputExists) {
      collides = fs::equivalent(input, output, error);
      if (error) {
        throw std::runtime_error("入出力パスの同一性を確認できません: " + pathToUtf8(output));
      }
    } else {
      collides = normalizedInput == normalizedPath(output);
    }

    if (collides) {
      throw std::runtime_error(
          "入力WAVと出力先が同じファイルです。原音を保護するため処理を中止しました: " +
          pathToUtf8(output));
    }
  }
}

class StagingDirectory {
public:
  explicit StagingDirectory(const fs::path& outputDirectory) {
    std::error_code error;
    fs::create_directories(outputDirectory, error);
    if (error) {
      throw std::runtime_error("出力フォルダを作成できません: " + pathToUtf8(outputDirectory));
    }

    std::mt19937_64 generator(std::random_device{}());
    for (int attempt = 0; attempt < 32; ++attempt) {
      std::ostringstream name;
      name << ".vvc-stage-" << std::hex << generator();
      fs::path candidate = outputDirectory / name.str();
      error.clear();
      if (fs::create_directory(candidate, error)) {
        path_ = std::move(candidate);
        return;
      }
      if (error && error != std::errc::file_exists) {
        throw std::runtime_error("一時フォルダを作成できません: " + error.message());
      }
    }
    throw std::runtime_error("衝突しない一時フォルダを作成できません。");
  }

  ~StagingDirectory() {
    if (!path_.empty()) {
      std::error_code error;
      fs::remove_all(path_, error);
    }
  }

  StagingDirectory(const StagingDirectory&) = delete;
  StagingDirectory& operator=(const StagingDirectory&) = delete;

  [[nodiscard]] const fs::path& path() const { return path_; }

private:
  fs::path path_;
};

void printUsage() {
  std::cout << "vvc_bench " VVC_VERSION " - 女声寄りDSPのローカル音質比較ベンチ\n\n"
            << "使い方:\n"
            << "  vvc_bench --input voice.wav --output-dir benchmark-output "
               "[--pitch 3.0] [--formant 2.0]\n\n"
            << "入力条件: 48 kHz / mono / PCM 16,24,32-bit または float 32-bit WAV\n"
            << "出力: dry.wav, pitch-only.wav, natural.wav, metrics.json\n"
            << "音声は録音・送信されず、指定した端末内でのみ処理されます。\n";
}

[[nodiscard]] Json signalJson(const std::vector<float>& samples) {
  const auto metrics = vvc::audio::measureSignal(samples);
  return {{"samples", samples.size()},
          {"peak", metrics.peak},
          {"rms", metrics.rms},
          {"clipped_samples", metrics.clipped_samples},
          {"non_finite_samples", metrics.non_finite_samples}};
}

void replaceFile(const fs::path& temporary, const fs::path& destination) {
  std::error_code error;
  fs::remove(destination, error);
  error.clear();
  fs::rename(temporary, destination, error);
  if (error) {
    throw std::runtime_error("出力ファイルを確定できません: " + destination.string() + " (" +
                             error.message() + ")");
  }
}

void writeJson(const fs::path& path, const Json& document) {
  std::ofstream stream(path, std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("metrics.jsonを作成できません。");
  }
  stream << document.dump(2) << '\n';
  if (!stream) {
    throw std::runtime_error("metrics.jsonの書き込みに失敗しました。");
  }
}

int run(const Arguments& arguments) {
  const auto input = vvc::audio::readMonoWav48k(arguments.input);
  const std::array outputPaths{
      arguments.output_dir / "dry.wav", arguments.output_dir / "pitch-only.wav",
      arguments.output_dir / "natural.wav", arguments.output_dir / "metrics.json"};
  ensureInputDoesNotCollide(arguments.input, outputPaths);

  const vvc::dsp::VoiceProcessor processor(input.sample_rate);
  const auto totalStarted = std::chrono::steady_clock::now();
  const auto comparison = processor.processComparison(input.samples, arguments.settings);
  const auto totalEnded = std::chrono::steady_clock::now();
  const double totalMilliseconds =
      std::chrono::duration<double, std::milli>(totalEnded - totalStarted).count();

  const StagingDirectory staging(arguments.output_dir);
  const fs::path dryTemporary = staging.path() / "dry.wav";
  const fs::path pitchTemporary = staging.path() / "pitch-only.wav";
  const fs::path naturalTemporary = staging.path() / "natural.wav";
  const fs::path metricsTemporary = staging.path() / "metrics.json";

  vvc::audio::writeMonoPcm16Wav(dryTemporary, {input.sample_rate, comparison.dry});
  vvc::audio::writeMonoPcm16Wav(pitchTemporary, {input.sample_rate, comparison.pitch_only});
  vvc::audio::writeMonoPcm16Wav(naturalTemporary, {input.sample_rate, comparison.natural});

  const Json metrics{
      {"schema_version", 1},
      {"tool", {{"name", "vvc_bench"}}},
      {"tool_version", VVC_VERSION},
      {"input",
       {{"filename", pathToUtf8(arguments.input.filename())},
        {"sample_rate_hz", input.sample_rate},
        {"channels", 1},
        {"samples", input.samples.size()}}},
      {"settings",
       {{"pitch_semitones", arguments.settings.pitch_semitones},
        {"formant_semitones", arguments.settings.formant_semitones},
        {"high_pass_hz", 80.0},
        {"gate_threshold_db", -52.0},
        {"compressor_threshold_db", -18.0},
        {"compressor_ratio", 3.0},
        {"limiter_ceiling_dbfs", -1.0}}},
      {"outputs",
       {{"dry", signalJson(comparison.dry)},
        {"pitch_only", signalJson(comparison.pitch_only)},
        {"natural", signalJson(comparison.natural)}}},
      {"diagnostics",
       {{"signalsmith_version", "1.3.2-pinned-57b93f4"},
        {"pitch_only",
         {{"processing_milliseconds", comparison.pitch_only_diagnostics.processing_milliseconds},
          {"input_latency_samples",
           comparison.pitch_only_diagnostics.signalsmith_input_latency_samples},
          {"output_latency_samples",
           comparison.pitch_only_diagnostics.signalsmith_output_latency_samples}}},
        {"natural",
         {{"processing_milliseconds", comparison.natural_diagnostics.processing_milliseconds},
          {"input_latency_samples",
           comparison.natural_diagnostics.signalsmith_input_latency_samples},
          {"output_latency_samples",
           comparison.natural_diagnostics.signalsmith_output_latency_samples}}},
        {"total_processing_milliseconds", totalMilliseconds}}},
      {"privacy",
       {{"network_used", false}, {"recording_performed", false}, {"telemetry_sent", false}}}};
  writeJson(metricsTemporary, metrics);

  replaceFile(dryTemporary, arguments.output_dir / "dry.wav");
  replaceFile(pitchTemporary, arguments.output_dir / "pitch-only.wav");
  replaceFile(naturalTemporary, arguments.output_dir / "natural.wav");
  replaceFile(metricsTemporary, arguments.output_dir / "metrics.json");

  std::cout << "比較ファイルを作成しました: " << pathToUtf8(arguments.output_dir) << '\n'
            << "次は docs/LISTENING_TEST.md に従ってブラインド試聴してください。\n";
  return 0;
}

int execute(const std::vector<fs::path>& commandLine) {
  try {
    const Arguments arguments = parseArguments(commandLine);
    if (arguments.show_help) {
      printUsage();
      return 0;
    }
    if (arguments.show_version) {
      std::cout << "vvc_bench " VVC_VERSION "\n";
      return 0;
    }
    return run(arguments);
  } catch (const std::exception& error) {
    std::cerr << "エラー: " << error.what() << '\n';
    return 1;
  }
}

} // namespace

#ifdef _WIN32
int wmain(const int argc, wchar_t** argv) {
  std::vector<fs::path> commandLine;
  commandLine.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    commandLine.emplace_back(argv[index]);
  }
  return execute(commandLine);
}
#else
int main(const int argc, char** argv) {
  std::vector<fs::path> commandLine;
  commandLine.reserve(static_cast<std::size_t>(argc));
  for (int index = 0; index < argc; ++index) {
    commandLine.emplace_back(argv[index]);
  }
  return execute(commandLine);
}
#endif
