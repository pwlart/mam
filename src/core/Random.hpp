#pragma once

#include <cstdint>
#include <string_view>

namespace mam::random {

// A node owns its stream. Unsigned fixed-width arithmetic and explicit float
// conversion keep the sequence independent of platform RNGs and scheduling.
class NoiseRandom {
public:
  static constexpr uint32_t defaultSeed = 0x12345678u;

  explicit NoiseRandom(uint32_t seed = defaultSeed) noexcept { setSeed(seed); }

  void setSeed(uint32_t seed) noexcept {
    initial_ = seed == 0 ? defaultSeed : seed;
    reset();
  }

  void reset() noexcept { state_ = initial_; }

  uint32_t nextUint32() noexcept {
    uint32_t value = state_;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    state_ = value;
    return value;
  }

  // High 24 bits map exactly to [-1, 1) in binary32, one draw per sample.
  float nextBipolar() noexcept {
    return static_cast<float>(nextUint32() >> 8) * (1.0f / 8388608.0f) - 1.0f;
  }

private:
  uint32_t initial_ = defaultSeed;
  uint32_t state_ = defaultSeed;
};

// Seed derivation v1, independent of std::hash, byte order and node insertion:
// FNV-1a32 over the ASCII domain, root LE32, scope length LE64 + raw bytes,
// then node-ID length LE64 + raw bytes; finish with the fixed fmix32 avalanche.
// Root zero requests instrument defaults. Derived streams never use the
// xorshift absorbing zero state. Renaming a scope or ID intentionally reseeds.
inline uint32_t deriveNodeSeed(uint32_t root, std::string_view scope,
                               std::string_view id) noexcept {
  if (root == 0) return 0;
  uint32_t hash = 2166136261u;
  const auto byte = [&](uint8_t value) noexcept {
    hash ^= value;
    hash *= 16777619u;
  };
  const auto littleEndian = [&](uint64_t value, unsigned count) noexcept {
    for (unsigned i = 0; i < count; ++i) {
      byte(static_cast<uint8_t>(value));
      value >>= 8;
    }
  };
  const auto field = [&](std::string_view value) noexcept {
    littleEndian(static_cast<uint64_t>(value.size()), 8);
    for (char character : value) byte(static_cast<uint8_t>(character));
  };
  for (char character : std::string_view("mam-node-seed-v1"))
    byte(static_cast<uint8_t>(character));
  littleEndian(root, 4);
  field(scope);
  field(id);
  hash ^= hash >> 16;
  hash *= 0x85ebca6bu;
  hash ^= hash >> 13;
  hash *= 0xc2b2ae35u;
  hash ^= hash >> 16;
  return hash == 0 ? NoiseRandom::defaultSeed : hash;
}

} // namespace mam::random
