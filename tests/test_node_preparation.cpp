#include "core/DelayNode.hpp"
#include "core/SpectralDuckerNode.hpp"
#include "AllocationWatch.hpp"
#include <doctest/doctest.h>
#include <array>
#include <limits>
#include <vector>

namespace {
std::vector<float> signal(uint32_t frames, uint32_t channels, SampleTime offset,
                          double frequency = 0.017) {
  std::vector<float> result(static_cast<size_t>(frames) * channels);
  for (uint32_t frame = 0; frame < frames; ++frame)
    for (uint32_t channel = 0; channel < channels; ++channel)
      result[static_cast<size_t>(frame) * channels + channel] =
          static_cast<float>(0.7 * std::sin(frequency * static_cast<double>(offset + frame) +
                                          static_cast<double>(channel) * 0.6));
  return result;
}

SpectralDuckerNode configuredDucker(SpectralDuckerNode::ApplyMode mode,
                                    SpectralDuckerNode::StereoMode stereoMode) {
  SpectralDuckerNode node;
  node.applyMode = mode;
  node.stereoMode = stereoMode;
  node.mix = .7f;
  node.scHpfHz = 70.0f;
  node.msSideScale = .3f;
  node.lookaheadMs = 1.7f;
  node.attackMs = 0.1f;
  for (auto& band : node.bands) {
    band.thresholdDb = -45.0f;
    band.holdMs = 3.0f;
  }
  return node;
}
}

TEST_CASE("prepared delay preserves legacy mono and stereo state across variable blocks") {
  for (const uint32_t channels : {1u, 2u}) {
    for (const double rate : {44100.0, 48000.0}) {
      CAPTURE(channels); CAPTURE(rate);
      DelayNode legacy, prepared;
      legacy.delayMs = prepared.delayMs = 7.3f;
      legacy.feedback = prepared.feedback = .6f;
      legacy.mix = prepared.mix = .8f;
      legacy.prepare(rate, 256);
      prepared.prepareRealtime(rate, 256, channels);
      SampleTime offset = 0;
      for (const uint32_t frames : {1u, 256u, 19u, 127u, 256u, 2u, 240u, 256u}) {
        auto expected = signal(frames, channels, offset);
        auto actual = expected;
        ProcessContext context{rate, frames, offset};
        legacy.processInPlace(context, expected.data(), channels);
        prepared.processInPlace(context, actual.data(), channels);
        CHECK(actual == expected);
        offset += frames;
      }
      legacy.reset(); prepared.reset();
      auto expected = signal(256, channels, 0);
      auto actual = expected;
      legacy.processInPlace({rate, 256, 0}, expected.data(), channels);
      prepared.processInPlace({rate, 256, 0}, actual.data(), channels);
      CHECK(actual == expected);
    }
  }
}

TEST_CASE("prepared spectral ducker preserves continuous legacy DSP in every mode") {
  for (const uint32_t channels : {1u, 2u}) {
    for (const double rate : {44100.0, 48000.0}) {
      for (const auto mode : {SpectralDuckerNode::ApplyMode::Multiply,
                              SpectralDuckerNode::ApplyMode::DynamicEq}) {
        for (const auto stereo : {SpectralDuckerNode::StereoMode::LR,
                                  SpectralDuckerNode::StereoMode::MidSide}) {
          CAPTURE(channels); CAPTURE(rate); CAPTURE(static_cast<int>(mode)); CAPTURE(static_cast<int>(stereo));
          auto legacy = configuredDucker(mode, stereo);
          auto prepared = configuredDucker(mode, stereo);
          legacy.prepare(rate, 256);
          prepared.prepareRealtime(rate, 256, channels);
          SampleTime offset = 0;
          for (const uint32_t frames : {1u, 256u, 19u, 127u, 256u, 2u, 240u, 256u}) {
            auto expected = signal(frames, channels, offset);
            auto actual = expected;
            const auto sidechain = signal(frames, channels, offset, .037);
            ProcessContext context{rate, frames, offset};
            legacy.applySidechain(context, expected.data(), sidechain.data(), channels);
            prepared.applySidechain(context, actual.data(), sidechain.data(), channels);
            CHECK(actual == expected);
            CHECK(std::all_of(actual.begin(), actual.end(), [](float x) { return std::isfinite(x); }));
            offset += frames;
          }
        }
      }
    }
  }
}

TEST_CASE("prepared spectral reset matches a fresh legacy insert for every segment") {
  for (const uint32_t channels : {1u, 2u}) {
    for (const auto mode : {SpectralDuckerNode::ApplyMode::Multiply,
                            SpectralDuckerNode::ApplyMode::DynamicEq}) {
      for (const auto stereo : {SpectralDuckerNode::StereoMode::LR,
                                SpectralDuckerNode::StereoMode::MidSide}) {
        CAPTURE(channels); CAPTURE(static_cast<int>(mode)); CAPTURE(static_cast<int>(stereo));
        auto prepared = configuredDucker(mode, stereo);
        prepared.prepareRealtime(48000.0, 512, channels);
        SampleTime offset = 0;
        for (const uint32_t frames : {256u, 1u, 512u, 37u, 200u, 511u}) {
          auto fresh = configuredDucker(mode, stereo);
          fresh.prepare(48000.0, std::min(frames, 4096u));
          auto expected = signal(frames, channels, offset);
          auto actual = expected;
          const auto sidechain = signal(frames, channels, offset, .037);
          ProcessContext context{48000.0, frames, offset};
          fresh.applySidechain(context, expected.data(), sidechain.data(), channels);
          prepared.reset();
          prepared.applySidechain(context, actual.data(), sidechain.data(), channels);
          CHECK(actual == expected);
          offset += frames;
        }
      }
    }
  }
}

TEST_CASE("prepared inserts silence unsupported capacity and layout without resizing") {
  DelayNode delay;
  SpectralDuckerNode ducker;
  delay.prepareRealtime(48000.0, 16, 2);
  ducker.prepareRealtime(48000.0, 16, 2);
  for (const ProcessContext context : {ProcessContext{48000.0, 17, 0},
                                       ProcessContext{44100.0, 16, 0}}) {
    auto a = signal(context.frames, 2, 20);
    auto b = a;
    const auto sidechain = a;
    delay.processInPlace(context, a.data(), 2);
    ducker.applySidechain(context, b.data(), sidechain.data(), 2);
    CHECK(std::all_of(a.begin(), a.end(), [](float x) { return x == 0; }));
    CHECK(std::all_of(b.begin(), b.end(), [](float x) { return x == 0; }));
  }
  auto mono = signal(16, 1, 20);
  auto other = mono;
  const auto detector = mono;
  delay.processInPlace({48000.0, 16, 0}, mono.data(), 1);
  ducker.applySidechain({48000.0, 16, 0}, other.data(), detector.data(), 1);
  CHECK(std::all_of(mono.begin(), mono.end(), [](float x) { return x == 0; }));
  CHECK(std::all_of(other.begin(), other.end(), [](float x) { return x == 0; }));

  delay.setDelayMs(1);
  ducker.bands.push_back({});
  auto a = signal(16, 2, 20);
  auto b = a;
  const auto sc = a;
  delay.processInPlace({48000.0, 16, 0}, a.data(), 2);
  ducker.applySidechain({48000.0, 16, 0}, b.data(), sc.data(), 2);
  CHECK(std::all_of(a.begin(), a.end(), [](float x) { return x == 0; }));
  CHECK(std::all_of(b.begin(), b.end(), [](float x) { return x == 0; }));
}

TEST_CASE("realtime insert preparation rejects invalid rate layout and arithmetic") {
  DelayNode delay;
  SpectralDuckerNode ducker;
  for (const auto rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()}) {
    CHECK_THROWS_AS(delay.prepareRealtime(rate, 256, 2), std::invalid_argument);
    CHECK_THROWS_AS(ducker.prepareRealtime(rate, 256, 2), std::invalid_argument);
  }
  for (const auto channels : {0u, 3u}) {
    CHECK_THROWS_AS(delay.prepareRealtime(48000, 256, channels), std::invalid_argument);
    CHECK_THROWS_AS(ducker.prepareRealtime(48000, 256, channels), std::invalid_argument);
  }
  CHECK_THROWS_AS(delay.prepareRealtime(48000, 0, 2), std::invalid_argument);
  CHECK_THROWS_AS(ducker.prepareRealtime(48000, 0, 2), std::invalid_argument);
  delay.delayMs = std::numeric_limits<float>::max();
  ducker.lookaheadMs = std::numeric_limits<float>::max();
  CHECK_THROWS_AS(delay.prepareRealtime(48000, 256, 2), std::invalid_argument);
  CHECK_THROWS_AS(ducker.prepareRealtime(48000, 256, 2), std::invalid_argument);
  ducker.lookaheadMs = 5;
  CHECK_THROWS_AS(ducker.prepareRealtime(48000, std::numeric_limits<uint32_t>::max(), 2),
                  std::invalid_argument);
}

TEST_CASE("prepared inserts allocate nothing on first variable and reset blocks") {
  for (const uint32_t channels : {1u, 2u}) {
    for (const auto mode : {SpectralDuckerNode::ApplyMode::Multiply,
                            SpectralDuckerNode::ApplyMode::DynamicEq}) {
      CAPTURE(channels); CAPTURE(static_cast<int>(mode));
      DelayNode delay;
      auto ducker = configuredDucker(mode, SpectralDuckerNode::StereoMode::MidSide);
      delay.prepareRealtime(48000, 256, channels);
      ducker.prepareRealtime(48000, 256, channels);
      std::array<float, 512> audio{};
      std::array<float, 512> detector{};
      for (size_t i = 0; i < audio.size(); ++i) {
        audio[i] = static_cast<float>(std::sin(static_cast<double>(i) * .017));
        detector[i] = static_cast<float>(std::sin(static_cast<double>(i) * .037));
      }
      mam::testing::AllocationCounts counts;
      {
        mam::testing::AllocationWatch watch(counts);
        for (const uint32_t frames : {1u, 256u, 17u, 128u}) {
          const ProcessContext context{48000, frames, 0};
          delay.processInPlace(context, audio.data(), channels);
          ducker.applySidechain(context, audio.data(), detector.data(), channels);
          delay.reset();
          ducker.reset();
        }
      }
      CHECK(counts.allocations == 0);
      CHECK(counts.deallocations == 0);
    }
  }
}
