#include "NativeAudio.hpp"
#include <AudioToolbox/ExtendedAudioFile.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace mam {
namespace {
void checked(OSStatus status, const char* operation) {
    if (status != noErr) throw std::runtime_error(std::string(operation) + ": " + std::to_string(status));
}
struct File {
    ExtAudioFileRef value = nullptr;
    ~File() { if (value) ExtAudioFileDispose(value); }
};
struct Url {
    CFURLRef value;
    explicit Url(const std::filesystem::path& path) {
        const auto text = path.string();
        value = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(text.data()), static_cast<CFIndex>(text.size()), false);
        if (!value) throw std::runtime_error("Cannot represent audio path");
    }
    ~Url() { CFRelease(value); }
};
AudioStreamBasicDescription floatFormat(uint32_t rate, uint32_t channels) {
    if (rate < 8000 || rate > 384000 || (channels != 1 && channels != 2))
        throw std::invalid_argument("Expected mono/stereo audio at 8..384 kHz");
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBitsPerChannel = 32;
    format.mChannelsPerFrame = channels;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 4 * channels;
    format.mBytesPerPacket = format.mBytesPerFrame;
    return format;
}
}

struct AudioReader::State {
    File file;
    AudioInfo info;
};
AudioReader::AudioReader(const std::filesystem::path& path) : state_(std::make_unique<State>()) {
    Url url(path);
    checked(ExtAudioFileOpenURL(url.value, &state_->file.value), "Open audio input");
    AudioStreamBasicDescription format{};
    UInt32 size = sizeof(format);
    checked(ExtAudioFileGetProperty(state_->file.value, kExtAudioFileProperty_FileDataFormat, &size, &format), "Read audio format");
    if (format.mFormatID != kAudioFormatLinearPCM || !std::isfinite(format.mSampleRate) ||
        format.mSampleRate < 8000 || format.mSampleRate > 384000 || std::floor(format.mSampleRate) != format.mSampleRate)
        throw std::invalid_argument("Native stem inputs must be PCM at an integer sample rate; decode compressed sources first");
    state_->info.sampleRate = static_cast<uint32_t>(format.mSampleRate);
    state_->info.channels = format.mChannelsPerFrame;
    SInt64 frames = 0;
    size = sizeof(frames);
    checked(ExtAudioFileGetProperty(state_->file.value, kExtAudioFileProperty_FileLengthFrames, &size, &frames), "Read audio length");
    if (frames < 0) throw std::runtime_error("Invalid audio length");
    state_->info.frames = static_cast<uint64_t>(frames);
    const auto client = floatFormat(state_->info.sampleRate, state_->info.channels);
    checked(ExtAudioFileSetProperty(state_->file.value, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client), "Set input Float32 format");
}
AudioReader::~AudioReader() = default;
const AudioInfo& AudioReader::info() const { return state_->info; }

void AudioReader::readAt(uint64_t startFrame, std::span<float> samples) {
    const auto channels = state_->info.channels;
    if (samples.size() % channels != 0) throw std::invalid_argument("Incomplete input frame");
    const uint64_t frames = samples.size() / channels;
    if (startFrame > state_->info.frames || frames > state_->info.frames - startFrame)
        throw std::out_of_range("Requested frames extend beyond audio input");
    if (samples.empty()) return;
    checked(ExtAudioFileSeek(state_->file.value, static_cast<SInt64>(startFrame)), "Seek audio input");
    uint64_t offset = 0;
    while (offset < frames) {
        UInt32 count = static_cast<UInt32>(std::min<uint64_t>(65536, frames - offset));
        AudioBufferList buffer{};
        buffer.mNumberBuffers = 1;
        buffer.mBuffers[0].mNumberChannels = channels;
        buffer.mBuffers[0].mDataByteSize = count * channels * 4;
        buffer.mBuffers[0].mData = samples.data() + offset * channels;
        checked(ExtAudioFileRead(state_->file.value, &count, &buffer), "Read audio frames");
        if (count == 0) throw std::runtime_error("Unexpected end of audio input");
        offset += count;
    }
    if (!std::all_of(samples.begin(), samples.end(), [](float value) { return std::isfinite(value); }))
        throw std::runtime_error("Audio input contains NaN or infinity");
}

struct FloatWaveWriter::State {
    File file;
    uint32_t channels = 0;
    uint64_t frames = 0;
};
uint64_t FloatWaveWriter::maximumFrames(uint32_t channels) {
    if (channels != 1 && channels != 2) throw std::invalid_argument("Expected mono/stereo output");
    return (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) - 4096) / (4 * channels);
}
FloatWaveWriter::FloatWaveWriter(const std::filesystem::path& path, uint32_t rate, uint32_t channels)
    : state_(std::make_unique<State>()) {
    const auto format = floatFormat(rate, channels);
    state_->channels = channels;
    Url url(path);
    checked(ExtAudioFileCreateWithURL(url.value, kAudioFileWAVEType, &format, nullptr, 0, &state_->file.value),
            "Create new WAV (existing destinations are refused)");
    checked(ExtAudioFileSetProperty(state_->file.value, kExtAudioFileProperty_ClientDataFormat, sizeof(format), &format), "Set output Float32 format");
}
FloatWaveWriter::~FloatWaveWriter() = default;
void FloatWaveWriter::append(std::span<const float> samples) {
    if (!state_->file.value) throw std::logic_error("Writer is closed");
    const auto channels = state_->channels;
    if (samples.size() % channels != 0) throw std::invalid_argument("Incomplete output frame");
    const uint64_t frames = samples.size() / channels;
    if (frames > maximumFrames(channels) - state_->frames)
        throw std::length_error("Output exceeds the supported RIFF WAV size");
    if (!std::all_of(samples.begin(), samples.end(), [](float value) { return std::isfinite(value); }))
        throw std::runtime_error("Refusing nonfinite output samples");
    uint64_t offset = 0;
    while (offset < frames) {
        const auto count = static_cast<UInt32>(std::min<uint64_t>(65536, frames - offset));
        AudioBufferList buffer{};
        buffer.mNumberBuffers = 1;
        buffer.mBuffers[0].mNumberChannels = channels;
        buffer.mBuffers[0].mDataByteSize = count * channels * 4;
        buffer.mBuffers[0].mData = const_cast<float*>(samples.data() + offset * channels);
        checked(ExtAudioFileWrite(state_->file.value, count, &buffer), "Write audio frames");
        offset += count;
    }
    state_->frames += frames;
}
void FloatWaveWriter::close() {
    if (!state_->file.value) return;
    auto file = state_->file.value;
    state_->file.value = nullptr;
    checked(ExtAudioFileDispose(file), "Finish WAV output");
}
}
