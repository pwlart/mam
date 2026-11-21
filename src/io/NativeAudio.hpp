#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace mam {
struct AudioInfo {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint64_t frames = 0;
};

// Offline PCM access. Conversion changes representation to Float32, never the
// sample rate or channel count. File I/O does not belong in audio callbacks.
class AudioReader {
public:
    explicit AudioReader(const std::filesystem::path& path);
    ~AudioReader();
    AudioReader(const AudioReader&) = delete;
    AudioReader& operator=(const AudioReader&) = delete;
    const AudioInfo& info() const;
    void readAt(uint64_t startFrame, std::span<float> interleaved);
private:
    struct State;
    std::unique_ptr<State> state_;
};

class FloatWaveWriter {
public:
    FloatWaveWriter(const std::filesystem::path& path, uint32_t rate, uint32_t channels);
    ~FloatWaveWriter();
    FloatWaveWriter(const FloatWaveWriter&) = delete;
    FloatWaveWriter& operator=(const FloatWaveWriter&) = delete;
    void append(std::span<const float> interleaved);
    void close();
    static uint64_t maximumFrames(uint32_t channels);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
