// This small test-only RIFF reader is independent of MAM's file writer/reader.
// It inspects generated fixtures, never production media or an audio device.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

struct Wave {
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint16_t format = 0;
    uint32_t rate = 0;
    size_t frames = 0;
    std::vector<float> samples;
};

Wave readWave(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "Cannot open " + path);
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), {}};
    auto u16 = [&](size_t offset) -> uint16_t {
        require(offset <= bytes.size() && bytes.size() - offset >= 2, "Truncated uint16");
        return static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset]) |
                                     static_cast<uint16_t>(bytes[offset + 1]) << 8);
    };
    auto u32 = [&](size_t offset) -> uint32_t {
        require(offset <= bytes.size() && bytes.size() - offset >= 4, "Truncated uint32");
        return static_cast<uint32_t>(bytes[offset]) |
               static_cast<uint32_t>(bytes[offset + 1]) << 8 |
               static_cast<uint32_t>(bytes[offset + 2]) << 16 |
               static_cast<uint32_t>(bytes[offset + 3]) << 24;
    };
    auto tag = [&](size_t offset, const std::string& expected) {
        require(offset <= bytes.size() && bytes.size() - offset >= expected.size(), "Truncated tag");
        return std::equal(expected.begin(), expected.end(), bytes.begin() + static_cast<ptrdiff_t>(offset));
    };
    require(bytes.size() >= 12 && tag(0, "RIFF") && tag(8, "WAVE"), "Expected RIFF WAVE");
    require(static_cast<uint64_t>(u32(4)) + 8 == bytes.size(), "RIFF size does not match file");
    Wave wave;
    size_t dataOffset = 0;
    size_t dataBytes = 0;
    uint16_t blockAlign = 0;
    bool hasFormat = false;
    bool hasData = false;
    for (size_t offset = 12; offset < bytes.size();) {
        require(bytes.size() - offset >= 8, "Truncated chunk header");
        const size_t length = u32(offset + 4);
        const size_t start = offset + 8;
        require(length <= bytes.size() - start, "Truncated chunk");
        if (tag(offset, "fmt ")) {
            require(!hasFormat && length >= 16, "Invalid or duplicate format chunk");
            hasFormat = true;
            wave.format = u16(start);
            wave.channels = u16(start + 2);
            wave.rate = u32(start + 4);
            blockAlign = u16(start + 12);
            wave.bits = u16(start + 14);
            if (wave.format == 0xfffe) {
                require(length >= 40, "Truncated extensible format");
                wave.format = u16(start + 24);
            }
        } else if (tag(offset, "data")) {
            require(!hasData, "Duplicate data chunk");
            hasData = true;
            dataOffset = start;
            dataBytes = length;
        }
        // CoreAudio can omit the pad byte for an odd-sized final data chunk
        // (for example 241 mono PCM24 frames). Allow only that terminal case;
        // internal chunks still require padding, and complete frames are
        // checked against blockAlign below.
        const size_t end = start + length;
        const bool unpaddedFinalData = tag(offset, "data") && end == bytes.size();
        offset = end + (unpaddedFinalData ? 0u : (length & 1u));
        require(offset <= bytes.size(), "Missing chunk padding");
    }
    require(hasFormat && hasData, "Missing format or data chunk");
    require(wave.channels > 0 && (wave.bits == 16 || wave.bits == 24 || wave.bits == 32), "Invalid fixture format");
    const size_t sampleBytes = wave.bits / 8;
    require(blockAlign == wave.channels * sampleBytes && dataBytes % blockAlign == 0, "Incomplete interleaved frame");
    require((wave.format == 3 && wave.bits == 32) || (wave.format == 1 && wave.bits != 32), "Unexpected PCM encoding");
    wave.frames = dataBytes / blockAlign;
    wave.samples.reserve(dataBytes / sampleBytes);
    for (size_t offset = dataOffset; offset < dataOffset + dataBytes; offset += sampleBytes) {
        float value = 0;
        if (wave.format == 3) {
            value = std::bit_cast<float>(u32(offset));
        } else if (wave.bits == 16) {
            value = static_cast<float>(std::bit_cast<int16_t>(u16(offset))) / 32768.0f;
        } else {
            const uint32_t raw = static_cast<uint32_t>(bytes[offset]) |
                                 static_cast<uint32_t>(bytes[offset + 1]) << 8 |
                                 static_cast<uint32_t>(bytes[offset + 2]) << 16;
            const int32_t signedValue = (raw & 0x800000u) != 0
                ? static_cast<int32_t>(raw) - 0x1000000 : static_cast<int32_t>(raw);
            value = static_cast<float>(signedValue) / 8388608.0f;
        }
        require(std::isfinite(value), "Nonfinite audio sample");
        wave.samples.push_back(value);
    }
    return wave;
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 7, "Expected output reference frames channels bits loop arguments");
        const auto actual = readWave(argv[1]);
        const auto reference = readWave(argv[2]);
        const auto frames = static_cast<size_t>(std::stoul(argv[3]));
        const auto channels = static_cast<uint16_t>(std::stoul(argv[4]));
        const auto bits = static_cast<uint16_t>(std::stoul(argv[5]));
        const bool loop = std::string(argv[6]) == "true";
        require(actual.channels == channels, "Exported channel count differs from session");
        require(actual.frames == frames, "Exported frame count differs from requested duration");
        require(actual.rate == 48000 && actual.bits == bits, "Wrong sample rate or bit depth");
        require(reference.channels == 2 && reference.frames == frames && reference.bits == 32 && reference.rate == 48000,
                "Graph reference layout differs from the independent fixture contract");
        const double tolerance = bits == 32 ? 0.0 : (bits == 16 ? 1.0 / 32768.0 : 1.0 / 8388608.0);
        float peak = 0;
        for (size_t frame = 0; frame < frames; ++frame) {
            // Literal 241-frame / three-loop fixture: 81, 80, 80 frames.
            const size_t sourceFrame = !loop || frame < 81 ? frame : (frame < 161 ? frame - 81 : frame - 161);
            const auto expected = reference.samples[sourceFrame * 2];
            require(reference.samples[sourceFrame * 2 + 1] == expected, "Kick reference must be dual mono");
            for (uint16_t channel = 0; channel < channels; ++channel) {
                const float value = actual.samples[frame * channels + channel];
                require(std::abs(static_cast<double>(value) - expected) <= tolerance,
                        "PCM sequence differs at frame " + std::to_string(frame) + ", channel " + std::to_string(channel));
                peak = std::max(peak, std::abs(value));
            }
        }
        require(peak > 0.01f, "Fixture unexpectedly silent");
        std::cout << "Verified " << frames << " frames, " << channels << " channels, " << bits << " bits\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
