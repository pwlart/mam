#include "StemRenderer.hpp"
#include "../io/NativeAudio.hpp"
#include "../core/Sha1.hpp"
#include "../tools/BuildInfo.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <set>
#include <span>
#include <stdexcept>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

void keys(const Json& value, std::initializer_list<std::string> allowed) {
    if (!value.is_object()) throw std::invalid_argument("Expected JSON object");
    const std::set<std::string> names(allowed);
    for (const auto& item : value.items())
        if (!names.contains(item.key())) throw std::invalid_argument("Unknown field: " + item.key());
}
uint64_t integer(const Json& value) {
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer() && value.get<int64_t>() >= 0) return static_cast<uint64_t>(value.get<int64_t>());
    throw std::invalid_argument("Expected nonnegative integer frame/count value");
}
uint64_t optionalInteger(const Json& value, const char* key, uint64_t fallback) {
    return value.contains(key) ? integer(value.at(key)) : fallback;
}
fs::path resolve(const fs::path& base, const std::string& text) {
    if (text.empty()) throw std::invalid_argument("Empty path");
    return fs::weakly_canonical(base / fs::path(text));
}
std::string hexDigest(sha1_detail::Ctx& context) {
    uint8_t digest[20];
    sha1_detail::finalize(context, digest);
    const char* hex = "0123456789abcdef";
    std::string output;
    for (uint8_t byte : digest) { output += hex[byte >> 4]; output += hex[byte & 15]; }
    return output;
}
std::string fileHash(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read source for provenance");
    sha1_detail::Ctx context; sha1_detail::init(context);
    std::vector<char> buffer(65536);
    while (stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || stream.gcount())
        sha1_detail::update(context, buffer.data(), static_cast<size_t>(stream.gcount()));
    if (stream.bad()) throw std::runtime_error("Source hashing failed");
    return hexDigest(context);
}
struct Stem {
    std::unique_ptr<mam::AudioReader> reader;
    uint64_t source, destination, frames, attack, release;
    float gain;
};
void publishReport(const fs::path& path, const Json& report) {
    const std::string text = report.dump(2) + '\n';
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (descriptor < 0) throw std::runtime_error("Cannot create new result manifest");
    size_t offset = 0;
    while (offset < text.size()) {
        const auto count = write(descriptor, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(descriptor); throw std::runtime_error("Cannot write result manifest"); }
        offset += static_cast<size_t>(count);
    }
    if (close(descriptor) != 0) throw std::runtime_error("Cannot close result manifest");
}
float envelope(uint64_t position, const Stem& stem) {
    double level = 1;
    if (stem.attack && position < stem.attack)
        level *= 0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(position) / static_cast<double>(stem.attack - 1));
    if (stem.release && position >= stem.frames - stem.release)
        level *= 0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(stem.frames - 1 - position) / static_cast<double>(stem.release - 1));
    return static_cast<float>(level) * stem.gain;
}
}

int runStemRenderer(int argc, char** argv) {
    try {
        if (argc == 1 && (std::string(argv[0]) == "--help" || std::string(argv[0]) == "-h")) {
            std::cout << "mam stems render JOB.json\nNative offline PCM renderer; outputs a new directory.\n";
            return 0;
        }
        if (argc != 2 || std::string(argv[0]) != "render")
            throw std::invalid_argument("Use 'mam stems render JOB.json'");
        const auto jobPath = fs::absolute(argv[1]);
        std::ifstream input(jobPath);
        if (!input) throw std::runtime_error("Cannot open stem job");
        std::vector<std::set<std::string>> objectKeys;
        const Json job = Json::parse(input, [&](int, Json::parse_event_t event, Json& parsed) {
            if (event == Json::parse_event_t::object_start) objectKeys.emplace_back();
            if (event == Json::parse_event_t::key && !objectKeys.back().insert(parsed.get<std::string>()).second)
                throw std::invalid_argument("Duplicate JSON key: " + parsed.get<std::string>());
            if (event == Json::parse_event_t::object_end) objectKeys.pop_back();
            return true;
        });
        keys(job, {"schema_version", "sample_rate", "channels", "frames", "block_frames", "output_dir", "stems"});
        if (integer(job.at("schema_version")) != 1) throw std::invalid_argument("Unsupported stem job version");
        const auto rate64 = integer(job.at("sample_rate"));
        const auto channels64 = integer(job.at("channels"));
        if ((rate64 != 44100 && rate64 != 48000) || (channels64 != 1 && channels64 != 2))
            throw std::invalid_argument("Stem renderer currently supports mono/stereo at 44100 or 48000 Hz");
        const auto rate = static_cast<uint32_t>(rate64), channels = static_cast<uint32_t>(channels64);
        const uint64_t frames = integer(job.at("frames"));
        const uint64_t block = optionalInteger(job, "block_frames", 4096);
        if (!frames || frames > mam::FloatWaveWriter::maximumFrames(channels) || !block || block > 65536)
            throw std::invalid_argument("Invalid output length, RIFF limit or block size");
        const auto output = resolve(jobPath.parent_path(), job.at("output_dir").get<std::string>());
        if (fs::exists(output)) throw std::invalid_argument("Output directory already exists");
        if (!job.at("stems").is_array() || job.at("stems").empty() || job.at("stems").size() > 1024)
            throw std::invalid_argument("Expected 1..1024 stems");
        std::vector<Stem> stems;
        std::map<fs::path, std::string> sources;
        for (const auto& item : job.at("stems")) {
            keys(item, {"path", "source_start_frame", "destination_start_frame", "frames", "gain_db", "fade_in_frames", "fade_out_frames"});
            const auto path = resolve(jobPath.parent_path(), item.at("path").get<std::string>());
            auto reader = std::make_unique<mam::AudioReader>(path);
            const auto info = reader->info();
            if (info.sampleRate != rate || info.channels != channels)
                throw std::invalid_argument("Stem rate/channels must match the output; prepare explicitly first");
            const uint64_t source = optionalInteger(item, "source_start_frame", 0);
            const uint64_t destination = optionalInteger(item, "destination_start_frame", 0);
            const uint64_t count = integer(item.at("frames"));
            const uint64_t attack = optionalInteger(item, "fade_in_frames", 0);
            const uint64_t release = optionalInteger(item, "fade_out_frames", 0);
            if (!count || source > info.frames || count > info.frames - source ||
                destination > frames || count > frames - destination || attack > count || release > count - attack ||
                attack == 1 || release == 1)
                throw std::invalid_argument("Invalid stem window, destination or fade lengths");
            const double gainDb = item.value("gain_db", 0.0);
            if (!std::isfinite(gainDb) || gainDb < -120 || gainDb > 24)
                throw std::invalid_argument("Gain must be finite and between -120 and 24 dB");
            if (!sources.contains(path)) sources.emplace(path, fileHash(path));
            stems.push_back({std::move(reader), source, destination, count, attack, release,
                             static_cast<float>(std::pow(10.0, gainDb / 20.0))});
        }
        // Create only after validating all inputs. A missing result.json marks
        // an incomplete/failed render; never treat a partial WAV as accepted.
        if (!fs::create_directory(output)) throw std::runtime_error("Cannot reserve output directory");
        mam::FloatWaveWriter writer(output / "audio.wav", rate, channels);
        std::vector<float> mix(static_cast<size_t>(block) * channels), buffer(mix.size());
        sha1_detail::Ctx sampleHash; sha1_detail::init(sampleHash);
        float peak = 0;
        for (uint64_t position = 0; position < frames;) {
            const uint64_t count = std::min(block, frames - position);
            std::fill(mix.begin(), mix.end(), 0);
            for (auto& stem : stems) {
                const uint64_t start = std::max(position, stem.destination);
                const uint64_t end = std::min(position + count, stem.destination + stem.frames);
                if (end <= start) continue;
                const size_t length = static_cast<size_t>(end - start);
                stem.reader->readAt(stem.source + start - stem.destination, std::span(buffer.data(), length * channels));
                for (size_t i = 0; i < length; ++i) {
                    const float gain = envelope(start - stem.destination + i, stem);
                    for (uint32_t channel = 0; channel < channels; ++channel)
                        mix[(start - position + i) * channels + channel] += buffer[i * channels + channel] * gain;
                }
            }
            const auto samples = std::span(mix.data(), static_cast<size_t>(count) * channels);
            for (float value : samples) peak = std::max(peak, std::abs(value));
            writer.append(samples);
            sha1_detail::update(sampleHash, samples.data(), samples.size_bytes());
            position += count;
        }
        writer.close();
        Json hashes = Json::object();
        for (const auto& [path, hash] : sources) {
            if (fileHash(path) != hash) throw std::runtime_error("Source changed during stem render");
            hashes[path.string()] = hash;
        }
        Json report{{"schema_version", 1}, {"status", "ok"}, {"operation", "native_stem_render"},
                    {"frames", frames}, {"sample_rate", rate}, {"channels", channels},
                    {"sample_peak", peak}, {"normalization", "none"}, {"source_file_sha1", hashes},
                    {"sample_sha1", hexDigest(sampleHash)}, {"job", job}, {"output_dir", output.string()}};
        report["build"] = Json::parse(mamBuildInfoJson());
        publishReport(output / "result.json", report);
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MAM stems: " << error.what() << '\n';
        std::cout << Json{{"status", "error"}, {"message", error.what()}}.dump() << '\n';
        return 1;
    }
}
