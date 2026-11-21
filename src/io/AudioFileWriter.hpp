#pragma once

#include <AudioToolbox/ExtendedAudioFile.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudioTypes.h>
#include <CoreFoundation/CoreFoundation.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <limits>

enum class FileFormat { Wav, Aiff, Caf };
enum class BitDepth { Pcm16, Pcm24, Float32 };

struct AudioFileSpec {
  FileFormat format = FileFormat::Wav;
  BitDepth bitDepth = BitDepth::Float32;
  uint32_t sampleRate = 48000;
  uint32_t channels = 2;
};

inline OSType toFileType(FileFormat f) {
  switch (f) {
    case FileFormat::Wav: return kAudioFileWAVEType;
    case FileFormat::Aiff: return kAudioFileAIFFType;
    case FileFormat::Caf: return kAudioFileCAFType;
  }
  return kAudioFileWAVEType;
}

inline void writeWithExtAudioFile(const std::string& path, const AudioFileSpec& spec, const std::vector<float>& interleaved) {
  if (spec.sampleRate == 0 || spec.channels == 0 ||
      spec.channels > std::numeric_limits<UInt32>::max() / sizeof(float) ||
      interleaved.size() % spec.channels != 0) {
    throw std::invalid_argument("Invalid audio format or incomplete interleaved frame");
  }
  struct ScopedExtAudioFile {
    ExtAudioFileRef file = nullptr;
    ~ScopedExtAudioFile() { if (file) ExtAudioFileDispose(file); }
    ScopedExtAudioFile() = default;
    ScopedExtAudioFile(const ScopedExtAudioFile&) = delete;
    ScopedExtAudioFile& operator=(const ScopedExtAudioFile&) = delete;
  };

  AudioStreamBasicDescription src{};
  src.mSampleRate = spec.sampleRate;
  src.mFormatID = kAudioFormatLinearPCM;
  src.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
  src.mBitsPerChannel = 32;
  src.mChannelsPerFrame = spec.channels;
  src.mFramesPerPacket = 1;
  src.mBytesPerFrame = 4 * spec.channels;
  src.mBytesPerPacket = src.mBytesPerFrame;

  AudioStreamBasicDescription dst = src;
  if (spec.bitDepth == BitDepth::Pcm16) {
    dst.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    dst.mBitsPerChannel = 16;
    dst.mBytesPerFrame = (dst.mBitsPerChannel / 8) * spec.channels;
    dst.mBytesPerPacket = dst.mBytesPerFrame;
  } else if (spec.bitDepth == BitDepth::Pcm24) {
    dst.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    dst.mBitsPerChannel = 24;
    dst.mBytesPerFrame = (dst.mBitsPerChannel / 8) * spec.channels;
    dst.mBytesPerPacket = dst.mBytesPerFrame;
  } else {
    dst = src; // Float32
  }

  // For AIFF integer formats, force big-endian
  if (spec.format == FileFormat::Aiff && spec.bitDepth != BitDepth::Float32) {
    dst.mFormatFlags |= kLinearPCMFormatFlagIsBigEndian;
  }

  CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.c_str()), static_cast<CFIndex>(path.size()), false);
  if (!url) {
    throw std::runtime_error("CFURLCreateFromFileSystemRepresentation failed");
  }
  ScopedExtAudioFile scoped;
  // No erase flag: CoreAudio must reject an existing destination, including
  // one created between a caller's check and this call.
  OSStatus err = ExtAudioFileCreateWithURL(url, toFileType(spec.format), &dst, nullptr, 0, &scoped.file);
  CFRelease(url);
  if (err != noErr) {
    throw std::runtime_error("Cannot create new audio file (destination may already exist): " + path + " [" + std::to_string(err) + "]");
  }

  err = ExtAudioFileSetProperty(scoped.file, kExtAudioFileProperty_ClientDataFormat, sizeof(src), &src);
  if (err != noErr) {
    throw std::runtime_error("ExtAudioFileSetProperty(ClientDataFormat) failed");
  }

  AudioBufferList buf{};
  buf.mNumberBuffers = 1;
  buf.mBuffers[0].mNumberChannels = spec.channels;
  const size_t totalFrames = interleaved.size() / spec.channels;
  const size_t chunkFrames = std::min<size_t>(65536, std::numeric_limits<UInt32>::max() / src.mBytesPerFrame);
  for (size_t offset = 0; offset < totalFrames;) {
    const auto frames = static_cast<UInt32>(std::min(chunkFrames, totalFrames - offset));
    buf.mBuffers[0].mDataByteSize = frames * src.mBytesPerFrame;
    buf.mBuffers[0].mData = const_cast<float*>(interleaved.data() + offset * spec.channels);
    err = ExtAudioFileWrite(scoped.file, frames, &buf);
    if (err != noErr) {
      throw std::runtime_error("ExtAudioFileWrite failed: " + std::to_string(err));
    }
    offset += frames;
  }
  err = ExtAudioFileDispose(scoped.file);
  scoped.file = nullptr;
  if (err != noErr) {
    throw std::runtime_error("ExtAudioFileDispose failed: " + std::to_string(err));
  }
}
