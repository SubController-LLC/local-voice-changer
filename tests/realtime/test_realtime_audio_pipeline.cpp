#include "realtime/RealtimeAudioPipeline.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <latch>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>

using vvc::realtime::PipelinePhase;
using vvc::realtime::RealtimeAudioPipeline;

namespace {

void copyProcessor(const std::span<const float> input, const std::span<float> output) {
  std::ranges::copy(input, output.begin());
}

} // namespace

TEST_CASE("realtime pipeline caps stale backlog and recovers after a stall") {
  RealtimeAudioPipeline pipeline;
  pipeline.prepare();
  std::array<float, RealtimeAudioPipeline::kBlockFrames> block{};
  block.fill(0.25F);

  // Withhold the worker and output consumer until the bound requests a reset.
  for (int tick = 0; tick < 8; ++tick) {
    (void)pipeline.pushInput(block);
  }
  auto stalled = pipeline.stats();
  REQUIRE(stalled.phase == PipelinePhase::reset_pending);
  REQUIRE(stalled.logical_depth == RealtimeAudioPipeline::kMaximumTotalQueueFrames);
  REQUIRE(stalled.max_logical_depth == RealtimeAudioPipeline::kMaximumTotalQueueFrames);
  REQUIRE(stalled.max_avoidable_backlog <= RealtimeAudioPipeline::kMaximumAvoidableBacklogFrames);
  REQUIRE(stalled.resynchronizations == 1U);

  std::array<float, RealtimeAudioPipeline::kBlockFrames> output{};
  REQUIRE(pipeline.popOutput(output) == 0U);
  REQUIRE(std::ranges::all_of(output, [](const float sample) { return sample == 0.0F; }));
  REQUIRE(pipeline.processOne(copyProcessor, [] {}) == true);
  REQUIRE(pipeline.stats().phase == PipelinePhase::running);

  // Simulate one second of normalized callback/worker ticks.
  for (int tick = 0; tick < 100; ++tick) {
    REQUIRE(pipeline.pushInput(block) == block.size());
    REQUIRE(pipeline.processOne(copyProcessor, [] {}) == true);
    REQUIRE(pipeline.popOutput(output) == output.size());
  }
  const auto recovered = pipeline.stats();
  REQUIRE(recovered.phase == PipelinePhase::running);
  REQUIRE(recovered.input_depth == 0U);
  REQUIRE(recovered.output_depth == RealtimeAudioPipeline::kNominalPrimingFrames);
  REQUIRE(recovered.logical_depth == RealtimeAudioPipeline::kNominalPrimingFrames);
  REQUIRE(recovered.avoidable_backlog == 0U);
}

TEST_CASE("realtime pipeline discards DSP output when resync starts during processing") {
  RealtimeAudioPipeline pipeline;
  pipeline.prepare();

  std::array<float, RealtimeAudioPipeline::kBlockFrames> block{};
  block.fill(0.25F);
  std::array<float, RealtimeAudioPipeline::kBlockFrames> output{};
  REQUIRE(pipeline.popOutput(output) == output.size());
  REQUIRE(pipeline.pushInput(block) == block.size());

  std::latch processorEntered{1};
  std::latch releaseProcessor{1};
  std::atomic<bool> processed{true};
  std::thread worker([&] {
    processed.store(pipeline.processOne(
                        [&](const std::span<const float>, const std::span<float> processedOutput) {
                          processorEntered.count_down();
                          releaseProcessor.wait();
                          std::ranges::fill(processedOutput, 0.75F);
                        },
                        [] {}),
                    std::memory_order_release);
  });

  processorEntered.wait();
  REQUIRE(pipeline.popOutput(output) == 0U);
  REQUIRE(pipeline.stats().phase == PipelinePhase::reset_pending);
  releaseProcessor.count_down();
  worker.join();

  REQUIRE_FALSE(processed.load(std::memory_order_acquire));
  REQUIRE(pipeline.stats().logical_depth == 0U);

  // A second callback acknowledges that the old output queue is drained. The reset then primes
  // only fresh silence; the 0.75 marker from the interrupted DSP generation is never published.
  REQUIRE(pipeline.popOutput(output) == 0U);
  REQUIRE(pipeline.processOne(copyProcessor, [] {}));
  REQUIRE(pipeline.stats().logical_depth == RealtimeAudioPipeline::kNominalPrimingFrames);
  REQUIRE(pipeline.popOutput(output) == output.size());
  REQUIRE(std::ranges::all_of(output, [](const float sample) { return sample == 0.0F; }));
}

TEST_CASE("realtime pipeline drains output published after the first reset callback") {
  RealtimeAudioPipeline pipeline;
  pipeline.prepare();

  std::array<float, RealtimeAudioPipeline::kBlockFrames> block{};
  std::array<float, RealtimeAudioPipeline::kBlockFrames> output{};
  REQUIRE(pipeline.popOutput(output) == output.size());
  REQUIRE(pipeline.pushInput(block) == block.size());

  std::latch publishGateReached{1};
  std::latch releasePublish{1};
  std::atomic<bool> processed{false};
  std::thread worker([&] {
    processed.store(pipeline.processOneWithPublishHookForTesting(
                        [](const std::span<const float>, const std::span<float> processedOutput) {
                          std::ranges::fill(processedOutput, 0.75F);
                        },
                        [] {},
                        [&] {
                          publishGateReached.count_down();
                          releasePublish.wait();
                        }),
                    std::memory_order_release);
  });

  // The worker has already observed `running`, but has not pushed its marker output yet.
  publishGateReached.wait();
  REQUIRE(pipeline.pushInput(block) == block.size());
  REQUIRE(pipeline.pushInput(block) == block.size());
  REQUIRE(pipeline.pushInput(block) == 0U);
  REQUIRE(pipeline.stats().phase == PipelinePhase::reset_pending);

  // This callback sees an empty output ring. It must not acknowledge the drain while the old
  // worker generation can still publish into that ring.
  REQUIRE(pipeline.popOutput(output) == 0U);
  releasePublish.count_down();
  worker.join();
  REQUIRE(processed.load(std::memory_order_acquire));
  REQUIRE(pipeline.stats().phase == PipelinePhase::reset_pending);
  REQUIRE(pipeline.stats().output_depth == RealtimeAudioPipeline::kBlockFrames);
  REQUIRE(pipeline.stats().logical_depth == RealtimeAudioPipeline::kMaximumTotalQueueFrames);

  // Reset cannot complete from the premature callback. A later callback drains the marker,
  // acknowledges this reset generation, and only then may the worker prime fresh silence.
  REQUIRE_FALSE(pipeline.processOne(copyProcessor, [] {}));
  REQUIRE(pipeline.stats().output_depth == RealtimeAudioPipeline::kBlockFrames);
  REQUIRE(pipeline.popOutput(output) == 0U);
  REQUIRE(pipeline.stats().output_depth == 0U);
  REQUIRE(pipeline.processOne(copyProcessor, [] {}));
  REQUIRE(pipeline.popOutput(output) == output.size());
  REQUIRE(std::ranges::all_of(output, [](const float sample) { return sample == 0.0F; }));
}

TEST_CASE("realtime pipeline includes in-flight DSP frames in the concurrent queue cap") {
  RealtimeAudioPipeline pipeline;
  pipeline.prepare();

  std::array<float, RealtimeAudioPipeline::kBlockFrames> block{};
  std::array<float, RealtimeAudioPipeline::kBlockFrames> output{};
  REQUIRE(pipeline.popOutput(output) == output.size());
  REQUIRE(pipeline.pushInput(block) == block.size());

  std::latch processorEntered{1};
  std::latch releaseProcessor{1};
  std::atomic<bool> processed{true};
  std::thread worker([&] {
    processed.store(
        pipeline.processOne(
            [&](const std::span<const float> input, const std::span<float> processedOutput) {
              processorEntered.count_down();
              releaseProcessor.wait();
              std::ranges::copy(input, processedOutput.begin());
            },
            [] {}),
        std::memory_order_release);
  });

  processorEntered.wait();
  REQUIRE(pipeline.stats().input_depth == 0U);
  REQUIRE(pipeline.stats().logical_depth == RealtimeAudioPipeline::kBlockFrames);
  REQUIRE(pipeline.pushInput(block) == block.size());
  REQUIRE(pipeline.pushInput(block) == block.size());
  REQUIRE(pipeline.stats().logical_depth == RealtimeAudioPipeline::kMaximumTotalQueueFrames);

  // The rings contain only 960 frames here, while another 480-frame block is inside DSP. A cap
  // based only on ring depths would incorrectly accept this block and reach 1,920 logical frames.
  REQUIRE(pipeline.pushInput(block) == 0U);
  const auto capped = pipeline.stats();
  REQUIRE(capped.phase == PipelinePhase::reset_pending);
  REQUIRE(capped.logical_depth == RealtimeAudioPipeline::kMaximumTotalQueueFrames);
  REQUIRE(capped.max_logical_depth == RealtimeAudioPipeline::kMaximumTotalQueueFrames);

  releaseProcessor.count_down();
  worker.join();
  REQUIRE_FALSE(processed.load(std::memory_order_acquire));
  REQUIRE(pipeline.stats().logical_depth == RealtimeAudioPipeline::kBlockFrames * 2U);

  REQUIRE(pipeline.popOutput(output) == 0U);
  REQUIRE(pipeline.processOne(copyProcessor, [] {}));
  const auto reset = pipeline.stats();
  REQUIRE(reset.phase == PipelinePhase::running);
  REQUIRE(reset.input_depth == 0U);
  REQUIRE(reset.output_depth == RealtimeAudioPipeline::kNominalPrimingFrames);
  REQUIRE(reset.logical_depth == RealtimeAudioPipeline::kNominalPrimingFrames);
  REQUIRE(reset.max_logical_depth <= RealtimeAudioPipeline::kMaximumTotalQueueFrames);
}

TEST_CASE("realtime pipeline resynchronizes an output underrun to silence") {
  RealtimeAudioPipeline pipeline;
  pipeline.prepare();
  std::array<float, RealtimeAudioPipeline::kBlockFrames * 2U> output{};
  REQUIRE(pipeline.popOutput(output) == RealtimeAudioPipeline::kNominalPrimingFrames);
  REQUIRE(pipeline.stats().phase == PipelinePhase::reset_pending);
  REQUIRE(pipeline.stats().output_underruns == 1U);
  REQUIRE(std::ranges::all_of(output, [](const float sample) { return sample == 0.0F; }));
}

TEST_CASE("realtime pipeline reports deadlines and fatal processing failures") {
  RealtimeAudioPipeline pipeline;
  pipeline.prepare();
  std::array<float, RealtimeAudioPipeline::kBlockFrames> block{};
  REQUIRE(pipeline.pushInput(block) == block.size());
  REQUIRE(pipeline.processOne(
      [](const std::span<const float>, const std::span<float>) {
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
      },
      [] {}));
  REQUIRE(pipeline.stats().deadline_misses == 1U);

  REQUIRE(pipeline.pushInput(block) == block.size());
  REQUIRE_FALSE(pipeline.processOne(
      [](const std::span<const float>, const std::span<float>) {
        throw std::runtime_error("synthetic DSP failure");
      },
      [] {}));
  REQUIRE(pipeline.hasFatalFailure());
  REQUIRE(pipeline.stats().fatal_dsp_failures == 1U);

  std::array<float, RealtimeAudioPipeline::kBlockFrames> output{};
  REQUIRE(pipeline.popOutput(output) == 0U);
  REQUIRE(std::ranges::all_of(output, [](const float sample) { return sample == 0.0F; }));
}

TEST_CASE("realtime pipeline sanitizes and limits worker output") {
  RealtimeAudioPipeline pipeline;
  pipeline.prepare();
  std::array<float, RealtimeAudioPipeline::kBlockFrames> input{};
  std::array<float, RealtimeAudioPipeline::kBlockFrames> priming{};
  REQUIRE(pipeline.popOutput(priming) == priming.size());
  REQUIRE(pipeline.pushInput(input) == input.size());
  REQUIRE(
      pipeline.processOne([](const std::span<const float>,
                             const std::span<float> output) { std::ranges::fill(output, 4.0F); },
                          [] {}));

  std::array<float, RealtimeAudioPipeline::kBlockFrames> output{};
  REQUIRE(pipeline.popOutput(output) == output.size());
  REQUIRE(std::ranges::all_of(output, [](const float sample) {
    return std::isfinite(sample) && sample <= 0.891'250'9F && sample >= -0.891'250'9F;
  }));
  REQUIRE(pipeline.stats().input_overruns == 0U);
}
