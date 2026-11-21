#include "io/NativeAudio.hpp"
#include <doctest/doctest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>
#include <unistd.h>

namespace {
struct NativeScratch {
    std::filesystem::path path;
    NativeScratch() {
        auto pattern = (std::filesystem::temp_directory_path() / "mam-native-XXXXXX").string();
        const char* created = mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create native test directory");
        path = created;
    }
    ~NativeScratch() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
}

TEST_CASE("native PCM reader seeks exactly across writer chunk boundaries") {
    NativeScratch scratch;
    const auto path = scratch.path / "source.wav";
    std::vector<float> source(131079 * 2);
    for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<float>(i % 997) / 1024.0f;
    mam::FloatWaveWriter writer(path, 44100, 2);
    writer.append(source);
    writer.close();
    mam::AudioReader reader(path);
    CHECK(reader.info().frames == 131079);
    CHECK(reader.info().sampleRate == 44100);
    CHECK(reader.info().channels == 2);
    for (uint64_t start : {0u, 65535u, 131072u, 1u}) {
        std::vector<float> actual(14);
        reader.readAt(start, actual);
        CHECK(std::equal(actual.begin(), actual.end(), source.begin() + static_cast<ptrdiff_t>(start * 2)));
    }
}

TEST_CASE("native reader rejects out of range and incomplete frame requests") {
    NativeScratch scratch;
    const auto path = scratch.path / "short.wav";
    mam::FloatWaveWriter writer(path, 48000, 2);
    writer.append(std::vector<float>(20, 0.25f));
    writer.close();
    mam::AudioReader reader(path);
    std::vector<float> stereo(4), partial(3);
    CHECK_THROWS_AS(reader.readAt(9, stereo), std::out_of_range);
    CHECK_THROWS_AS(reader.readAt(std::numeric_limits<uint64_t>::max(), stereo), std::out_of_range);
    CHECK_THROWS_AS(reader.readAt(0, partial), std::invalid_argument);
    CHECK_NOTHROW(reader.readAt(10, std::span<float>{}));
}

TEST_CASE("native stream writer refuses existing files and nonfinite samples") {
    NativeScratch scratch;
    const auto protectedPath = scratch.path / "protected.wav";
    { std::ofstream file(protectedPath); file << "unchanged"; }
    CHECK_THROWS_AS(mam::FloatWaveWriter(protectedPath, 44100, 2), std::runtime_error);
    CHECK(std::filesystem::file_size(protectedPath) == 9);
    mam::FloatWaveWriter writer(scratch.path / "new.wav", 44100, 2);
    CHECK_THROWS_AS(writer.append(std::vector<float>{0, std::numeric_limits<float>::quiet_NaN()}), std::runtime_error);
    CHECK_THROWS_AS(writer.append(std::vector<float>{0}), std::invalid_argument);
    writer.append(std::vector<float>{0.25f, -0.25f});
    writer.close();
    CHECK_THROWS_AS(writer.append(std::vector<float>{0, 0}), std::logic_error);
}

TEST_CASE("native PCM input rejects missing or corrupt files") {
    NativeScratch scratch;
    CHECK_THROWS_AS(mam::AudioReader(scratch.path / "absent.wav"), std::runtime_error);
    const auto path = scratch.path / "corrupt.wav";
    { std::ofstream file(path); file << "not an audio header"; }
    CHECK_THROWS_AS(mam::AudioReader{path}, std::runtime_error);
}
