#include "core/Random.hpp"
#include "instruments/mam_chip/MamChipNode.hpp"
#include "instruments/clap/ClapNode.hpp"
#include "AllocationWatch.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cmath>

TEST_CASE("prepared chip and clap own allocation free noise and reset state") {
  MamChipNode chip;
  ClapNode clap(ClapParams{});
  Node& chipNode = chip;
  Node& clapNode = clap;
  chipNode.setRandomSeed(mam::random::deriveNodeSeed(79, "live", "chip"));
  clapNode.setRandomSeed(mam::random::deriveNodeSeed(79, "live", "clap"));
  chip.prepare(48000, 128);
  clap.prepare(48000, 128);
  Command noise;
  noise.type = CommandType::SetParam;
  noise.paramId = 11;
  noise.value = 1;
  chip.handleEvent(noise);
  Command trigger;
  trigger.type = CommandType::Trigger;
  std::array<float, 256> chipOutput{}, clapOutput{};
  mam::testing::AllocationCounts counts;
  {
    mam::testing::AllocationWatch watch(counts);
    for (const uint32_t frames : {1u, 128u, 7u, 64u}) {
      chip.handleEvent(trigger); clap.handleEvent(trigger);
      chip.process({48000, frames, 0}, chipOutput.data(), 2);
      clap.process({48000, frames, 0}, clapOutput.data(), 2);
      chip.reset(); clap.reset();
    }
  }
  CHECK(counts.allocations == 0);
  CHECK(counts.deallocations == 0);
  CHECK(std::all_of(chipOutput.begin(), chipOutput.end(), [](float x) { return std::isfinite(x); }));
  CHECK(std::all_of(clapOutput.begin(), clapOutput.end(), [](float x) { return std::isfinite(x); }));
}
