#include "core/Random.hpp"
#include "instruments/mam_chip/MamChipNode.hpp"
#include "instruments/clap/ClapNode.hpp"
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string_view>
#include <vector>

namespace {
using mam::random::NoiseRandom;
using mam::random::deriveNodeSeed;

// Independently calculated from the published xorshift32 and high-24-bit
// mapping contract. Hexadecimal floats express exact binary32 values.
constexpr std::array<uint32_t, 10> defaultWords{
    0x87985aa5u, 0x155b24a3u, 0x4820f4c4u, 0x81b3ac98u, 0x703a0788u,
    0x29a8e24du, 0x89ca4f1du, 0xc5186e29u, 0xd37862a7u, 0x3ab14b11u};
constexpr std::array<float, 10> defaultBipolar{
    0x1.e6168p-5f, -0x1.aa937p-1f, -0x1.bef86p-2f, 0x1.b3acp-7f,
    -0x1.f8bf2p-4f, -0x1.595c78p-1f, 0x1.3949ep-4f, 0x1.1461b8p-1f,
    0x1.4de188p-1f, -0x1.153ad4p-1f};

// Original default clap output at 48 kHz, captured from its pre-migration
// xorshift/noise/envelope arithmetic. The old high-23-bit mapping is biased
// negative; correcting that would be an audible DSP change outside this task.
constexpr std::array<float, 16> legacyClap{
    -0x1.b1751ep-2f, -0x1.a64f6ep-1f, -0x1.4ae444p-1f, -0x1.c683f4p-2f,
    -0x1.02ac16p-1f, -0x1.8196dep-1f, -0x1.a94276p-2f, -0x1.a7c4fp-3f,
    -0x1.40513cp-3f, -0x1.62c8ap-1f, -0x1.0e247cp-1f, -0x1.3fe2dp-1f,
    -0x1.7699ap-7f, -0x1.6c2296p-1f, -0x1.9e21f6p-2f, -0x1.06f11p-5f};

void trigger(Node& node) {
  Command command;
  command.type = CommandType::Trigger;
  node.handleEvent(command);
}

void set(Node& node, uint16_t id, float value) {
  Command command;
  command.type = CommandType::SetParam;
  command.paramId = id;
  command.value = value;
  node.handleEvent(command);
}

void prepareNoiseChip(MamChipNode& chip, uint32_t seed) {
  Node& node = chip; // Exercise the shared virtual seeding contract.
  node.setRandomSeed(seed);
  chip.prepare(48000, 512);
  set(chip, 5, 1.0f);  // gain
  set(chip, 7, .001f); // reach unity on the first sample
  set(chip, 9, 1.0f);  // sustain unity
  set(chip, 11, 1.0f); // pure noise makes the RNG stream directly observable
  trigger(chip);
}

void renderParts(Node& node, std::span<float> output, uint32_t channels,
                 std::span<const uint32_t> blocks) {
  size_t offset = 0;
  for (const uint32_t frames : blocks) {
    node.process({48000, frames, offset / channels}, output.data() + offset, channels);
    offset += static_cast<size_t>(frames) * channels;
  }
  REQUIRE(offset == output.size());
}
}

TEST_CASE("owned noise has fixed integer and bipolar known-answer vectors") {
  NoiseRandom integers;
  NoiseRandom bipolar;
  for (size_t i = 0; i < defaultWords.size(); ++i) {
    CHECK(integers.nextUint32() == defaultWords[i]);
    CHECK(bipolar.nextBipolar() == defaultBipolar[i]);
  }
}

TEST_CASE("owned noise resets configured seeds and maps zero to a useful default") {
  NoiseRandom zero(0), defaults(NoiseRandom::defaultSeed), changed(79);
  std::array<uint32_t, 64> first{};
  bool differs = false;
  for (auto& value : first) {
    value = changed.nextUint32();
    const auto defaultValue = defaults.nextUint32();
    CHECK(zero.nextUint32() == defaultValue);
    differs = differs || value != defaultValue;
  }
  CHECK(differs);
  changed.reset();
  for (const auto expected : first) CHECK(changed.nextUint32() == expected);
  changed.setSeed(0);
  for (const auto expected : defaultWords) CHECK(changed.nextUint32() == expected);
  changed.setSeed(79);
  for (const auto expected : first) CHECK(changed.nextUint32() == expected);

  NoiseRandom range(1);
  float minimum = 1, maximum = -1;
  bool valid = true;
  for (size_t i = 0; i < 20000; ++i) {
    const float value = range.nextBipolar();
    valid = valid && std::isfinite(value) && value >= -1 && value < 1;
    minimum = std::min(minimum, value); maximum = std::max(maximum, value);
  }
  CHECK(valid);
  CHECK(minimum < -.99f);
  CHECK(maximum > .99f);
}

TEST_CASE("node seed derivation matches versioned length-delimited byte vectors") {
  // Generated independently with Python's struct.pack LE32/LE64, FNV-1a32
  // and the published fmix32 constants, rather than the production helper.
  CHECK(deriveNodeSeed(1, "", "") == 0xb75eff2du);
  CHECK(deriveNodeSeed(42, "rack:A", "clap") == 0x84cd1071u);
  CHECK(deriveNodeSeed(42, "rack:B", "clap") == 0x89cae9b2u);
  CHECK(deriveNodeSeed(42, "a", "bc") == 0x68f98bccu);
  CHECK(deriveNodeSeed(42, "ab", "c") == 0xd79eb718u);
  CHECK(deriveNodeSeed(0xffffffffu, "rack", "noise") == 0xec3e1cdau);
  CHECK(deriveNodeSeed(7, std::string_view("a\0b", 3), "c") == 0x9b7e71aau);
  CHECK(deriveNodeSeed(7, "a", std::string_view("\0bc", 3)) == 0x89fbd1ecu);
  CHECK(deriveNodeSeed(0x12345678u, "Wien/\xc3\x96" "1", "klang/\xc3\xa9") == 0xd3d61b6fu);
  CHECK(deriveNodeSeed(0, "rack:A", "clap") == 0);
  CHECK(deriveNodeSeed(0, "", "") == 0);
}

TEST_CASE("chip reset and preparation replay its stream while trigger continues it") {
  MamChipNode chip;
  prepareNoiseChip(chip, 0);
  std::array<float, 10> first{};
  chip.process({48000, 5, 0}, first.data(), 1);
  trigger(chip);
  chip.process({48000, 5, 5}, first.data() + 5, 1);
  CHECK(first == defaultBipolar);
  chip.reset(); trigger(chip);
  std::array<float, 10> replay{};
  chip.process({48000, 10, 0}, replay.data(), 1);
  CHECK(replay == first);
  chip.prepare(48000, 512); trigger(chip);
  chip.process({48000, 10, 0}, replay.data(), 1);
  CHECK(replay == first);

  chip.setRandomSeed(79); chip.reset(); trigger(chip);
  chip.process({48000, 10, 0}, replay.data(), 1);
  CHECK(replay != first);
  chip.setRandomSeed(0); chip.reset(); trigger(chip);
  chip.process({48000, 10, 0}, replay.data(), 1);
  CHECK(replay == first);
}

TEST_CASE("chip noise is independent of block partition and other sounding nodes") {
  for (const uint32_t channels : {1u, 2u}) {
    CAPTURE(channels);
    MamChipNode reference, partitioned, other;
    prepareNoiseChip(reference, 123);
    prepareNoiseChip(partitioned, 123);
    prepareNoiseChip(other, 999);
    ClapNode clap(ClapParams{});
    clap.setRandomSeed(12); clap.prepare(48000, 512); trigger(clap);
    std::vector<float> expected(512 * channels), actual(expected.size()), scratch(expected.size());
    renderParts(reference, expected, channels, std::array<uint32_t, 1>{512});
    size_t offset = 0;
    for (const uint32_t frames : {1u, 127u, 3u, 256u, 125u}) {
      other.process({48000, frames, offset}, scratch.data(), channels);
      clap.process({48000, frames, offset}, scratch.data(), channels);
      partitioned.process({48000, frames, offset}, actual.data() + offset * channels, channels);
      offset += frames;
    }
    CHECK(actual == expected);
  }
}

TEST_CASE("default clap retains its legacy biased audio and configured seed reset") {
  ClapSynth clap(ClapParams{}, 48000);
  clap.trigger();
  for (const auto expected : legacyClap) CHECK(clap.process() == expected);
  clap.trigger(); // Restart the envelope, retaining the noise cursor.
  for (const float expected : {-0x1.6b413p-3f, -0x1.f4f1eap-5f,
                                -0x1.5cfa4ep-4f, -0x1.581872p-1f})
    CHECK(clap.process() == expected);
  clap.reset(); clap.trigger();
  for (const auto expected : legacyClap) CHECK(clap.process() == expected);
  clap.setRandomSeed(79); clap.reset(); clap.trigger();
  std::array<float, 16> seeded{};
  for (auto& value : seeded) value = clap.process();
  CHECK(seeded != legacyClap);
  clap.reset(); clap.trigger();
  for (const auto expected : seeded) CHECK(clap.process() == expected);
  clap.setRandomSeed(0); clap.reset(); clap.trigger();
  for (const auto expected : legacyClap) CHECK(clap.process() == expected);
  bool allNonpositive = true;
  for (size_t i = 0; i < 2048; ++i) allNonpositive = allNonpositive && clap.process() <= 0;
  CHECK(allNonpositive);
}

TEST_CASE("clap noise is independent of block partition and other sounding nodes") {
  for (const uint32_t channels : {1u, 2u}) {
    CAPTURE(channels);
    ClapNode reference(ClapParams{}), partitioned(ClapParams{}), other(ClapParams{});
    for (Node* node : {static_cast<Node*>(&reference), static_cast<Node*>(&partitioned)}) {
      node->setRandomSeed(123); node->prepare(48000, 512); node->reset(); trigger(*node);
    }
    other.setRandomSeed(999); other.prepare(48000, 512); trigger(other);
    MamChipNode chip; prepareNoiseChip(chip, 12);
    std::vector<float> expected(512 * channels), actual(expected.size()), scratch(expected.size());
    renderParts(reference, expected, channels, std::array<uint32_t, 1>{512});
    size_t offset = 0;
    for (const uint32_t frames : {1u, 127u, 3u, 256u, 125u}) {
      other.process({48000, frames, offset}, scratch.data(), channels);
      chip.process({48000, frames, offset}, scratch.data(), channels);
      partitioned.process({48000, frames, offset}, actual.data() + offset * channels, channels);
      offset += frames;
    }
    CHECK(actual == expected);
    // Preparing a clap again has historically retained its active/noise state.
    reference.prepare(48000, 512);
    std::array<float, 2> nextReference{}, nextPartitioned{};
    reference.process({48000, 1, 512}, nextReference.data(), channels);
    partitioned.process({48000, 1, 512}, nextPartitioned.data(), channels);
    CHECK(nextReference == nextPartitioned);
  }
}
