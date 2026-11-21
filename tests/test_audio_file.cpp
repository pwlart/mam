#include "io/AudioFileWriter.hpp"
#include <doctest/doctest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cmath>

namespace {
class TempDirectory {
public:
    TempDirectory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "mam-tests-XXXXXX").string();
        const char* result = mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("Cannot create test scratch directory");
        path = result;
    }
    ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    std::filesystem::path path;
};

struct InputFile {
    ExtAudioFileRef file = nullptr;
    ~InputFile() { if (file) ExtAudioFileDispose(file); }
};

std::vector<float> readBack(const std::string& path, uint32_t rate, size_t frames) {
    const auto url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(path.data()), static_cast<CFIndex>(path.size()), false);
    REQUIRE(url != nullptr);
    InputFile input;
    const OSStatus opened = ExtAudioFileOpenURL(url, &input.file);
    CFRelease(url);
    REQUIRE(opened == noErr);
    SInt64 length = 0;
    UInt32 size = sizeof(length);
    REQUIRE(ExtAudioFileGetProperty(input.file, kExtAudioFileProperty_FileLengthFrames, &size, &length) == noErr);
    REQUIRE(length == static_cast<SInt64>(frames));
    AudioStreamBasicDescription format{};
    size = sizeof(format);
    REQUIRE(ExtAudioFileGetProperty(input.file, kExtAudioFileProperty_FileDataFormat, &size, &format) == noErr);
    CHECK(format.mSampleRate == static_cast<double>(rate));
    REQUIRE(format.mChannelsPerFrame == 2);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 8;
    format.mBytesPerPacket = 8;
    REQUIRE(ExtAudioFileSetProperty(input.file, kExtAudioFileProperty_ClientDataFormat, sizeof(format), &format) == noErr);
    std::vector<float> data(frames * 2);
    AudioBufferList buffer{};
    buffer.mNumberBuffers = 1;
    buffer.mBuffers[0].mNumberChannels = 2;
    size_t offset = 0;
    while (offset < frames) {
        UInt32 requested = static_cast<UInt32>(std::min<size_t>(8192, frames - offset));
        buffer.mBuffers[0].mDataByteSize = requested * 2 * sizeof(float);
        buffer.mBuffers[0].mData = data.data() + offset * 2;
        REQUIRE(ExtAudioFileRead(input.file, &requested, &buffer) == noErr);
        REQUIRE(requested > 0);
        offset += requested;
    }
    REQUIRE(offset == frames);
    return data;
}

std::vector<float> fixture(size_t frames) {
    std::vector<float> data(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        data[2 * i] = static_cast<float>(i % 256) / 512.0f;
        data[2 * i + 1] = -static_cast<float>(i % 128) / 256.0f;
    }
    return data;
}
}

TEST_CASE("float WAV preserves every stereo sample across write chunks") {
    TempDirectory directory;
    const auto samples = fixture(131073);
    AudioFileSpec spec;
    spec.sampleRate = 44100;
    const auto path = (directory.path / "float.wav").string();
    writeWithExtAudioFile(path, spec, samples);
    const auto decoded = readBack(path, spec.sampleRate, samples.size() / 2);
    CHECK(decoded == samples);
}

TEST_CASE("integer WAV preserves representable stereo samples") {
    for (const auto depth : {BitDepth::Pcm16, BitDepth::Pcm24}) {
        TempDirectory directory;
        AudioFileSpec spec;
        spec.bitDepth = depth;
        const auto samples = fixture(1001);
        const auto path = (directory.path / "integer.wav").string();
        writeWithExtAudioFile(path, spec, samples);
        const auto decoded = readBack(path, spec.sampleRate, samples.size() / 2);
        REQUIRE(decoded.size() == samples.size());
        const double tolerance = depth == BitDepth::Pcm16 ? 1.0 / 32768 : 1.0 / 8388608;
        for (size_t i = 0; i < samples.size(); ++i)
            CHECK(std::abs(static_cast<double>(decoded[i]) - samples[i]) <= tolerance);
    }
}

TEST_CASE("export refuses to overwrite an existing file") {
    TempDirectory directory;
    const auto path = directory.path / "protected.wav";
    const std::string original = "previous master must survive";
    { std::ofstream output(path, std::ios::binary); output << original; }
    CHECK_THROWS_AS(writeWithExtAudioFile(path.string(), AudioFileSpec{}, fixture(10)), std::runtime_error);
    std::ifstream input(path, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(input), {}) == original);
}

TEST_CASE("invalid export formats fail before creating a file") {
    TempDirectory directory;
    AudioFileSpec spec;
    auto samples = fixture(10);
    SUBCASE("zero channels") { spec.channels = 0; }
    SUBCASE("zero sample rate") { spec.sampleRate = 0; }
    SUBCASE("incomplete stereo frame") { samples.pop_back(); }
    const auto path = directory.path / "invalid.wav";
    CHECK_THROWS_AS(writeWithExtAudioFile(path.string(), spec, samples), std::invalid_argument);
    CHECK_FALSE(std::filesystem::exists(path));
}
