#pragma once

#include <array>
#include <span>

namespace vvc::dsp {

class FormantShifter {
public:
  explicit FormantShifter(float sampleRate = 48'000.0F);

  void reset();
  void process(std::span<float> samples, float semitones);

private:
  struct PeakingFilter {
    void configure(float sampleRate, float frequency, float quality, float gainDb);
    [[nodiscard]] float process(float input);
    void reset();

    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double z1 = 0.0;
    double z2 = 0.0;
  };

  void updateFilters(float semitones);

  float sample_rate_;
  float configured_semitones_ = 0.0F;
  bool configured_ = false;
  std::array<PeakingFilter, 4> cuts_;
  std::array<PeakingFilter, 4> boosts_;
};

} // namespace vvc::dsp
