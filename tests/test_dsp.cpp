#include "instruments/kick/KickSynth.hpp"
#include "core/Node.hpp"
#include "core/Sha1.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cmath>
#include <string>

TEST_CASE("kick decays to silence without nonfinite samples at production rates") {
    for (const int rate : {44100, 48000}) {
        CAPTURE(rate);
        KickParams parameters;
        parameters.ampDecayMs = 50.0f;
        KickSynth kick(parameters, rate);
        double earlyEnergy = 0.0, lateEnergy = 0.0;
        bool finite = true;
        float last = 1.0f;
        for (int i = 0; i < rate; ++i) {
            last = kick.process();
            finite = finite && std::isfinite(last);
            if (i < rate / 10) earlyEnergy += last * last;
            if (i >= rate / 2) lateEnergy += last * last;
        }
        CHECK(finite);
        CHECK(earlyEnergy > 1.0);
        CHECK(lateEnergy < earlyEnergy * 0.0001);
        CHECK(last == 0.0f);
    }
}

TEST_CASE("kick velocity preserves its waveform and clamps to the valid range") {
    KickParams parameters;
    KickSynth full(parameters, 44100), half(parameters, 44100), silent(parameters, 44100), clipped(parameters, 44100);
    full.trigger(1.0f); half.trigger(0.5f); silent.trigger(-1.0f); clipped.trigger(2.0f);
    for (int i = 0; i < 512; ++i) {
        const float reference = full.process();
        CHECK(half.process() == reference * 0.5f);
        CHECK(silent.process() == 0.0f);
        CHECK(clipped.process() == reference);
    }
}

TEST_CASE("kick reset reproduces the same onset") {
    KickSynth kick(KickParams{}, 48000);
    std::array<float, 512> onset{};
    for (auto& value : onset) value = kick.process();
    kick.reset();
    for (const float expected : onset) CHECK(kick.process() == expected);
}

TEST_CASE("meters measure both stereo channels and handle silence") {
    const std::array<float, 4> stereo{0.5f, -0.5f, 0.5f, -0.5f};
    double peak = 0.0, rms = 0.0;
    measurePeakRms(stereo.data(), 2, 2, peak, rms);
    CHECK(peak == 0.5);
    CHECK(rms == 0.5);
    measurePeakRms(nullptr, 0, 2, peak, rms);
    CHECK(peak == 0.0);
    CHECK(rms == 0.0);
}

TEST_CASE("sample hash agrees with standard SHA1 vectors") {
    CHECK(computeSha1Hex("", 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(computeSha1Hex("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    const std::string multiBlock = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(computeSha1Hex(multiBlock.data(), multiBlock.size()) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}
