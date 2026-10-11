// Skate 3 custom music: replaces in-game iPod songs with user audio at the XMA
// decoder output. Game files are never modified.
//
// How it works
//   tools/skate3_music_import (`prepare`) writes a folder containing:
//     ipod.s3fp     fingerprints of every XMA packet in Ipod_Stream.mus, each mapped to
//                   (song slot, sample position within that song)
//     manifest.tsv  song slot -> user PCM file (44.1 kHz stereo s16le)
//     NNN.pcm       converted user tracks
//   The runtime calls Engine::OnFrame for every decoded XMA frame. If the frame's source
//   packet is an iPod packet, the 512 decoded samples are replaced with the user track at
//   the same position. Everything else (SFX, other music) passes through untouched.
//
// This file is pure C++17 with no rexglue dependency so it can be unit-tested standalone.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace skate3::custom_music {

constexpr uint32_t kFingerprintSkip = 4;    // skip XMA packet header
constexpr uint32_t kFingerprintLength = 256;
constexpr uint32_t kSamplesPerFrame = 512;  // per channel, matches XmaContext
constexpr uint32_t kIndexVersion = 3;

// UTF-8 <-> path conversion that works in C++17 and C++20+ (char8_t).
std::string PathToUtf8(const std::filesystem::path& p);
std::filesystem::path PathFromUtf8(const std::string& s);

// FNV-1a 64 over packet[kFingerprintSkip, kFingerprintSkip + kFingerprintLength).
uint64_t Fingerprint(const uint8_t* packet);

struct Stats {
  uint64_t frames_seen = 0;        // frames offered to OnFrame
  uint64_t frames_matched = 0;     // frames whose packet was an iPod packet
  uint64_t frames_continued = 0;   // unmatched frames bridged on an active iPod context
  uint64_t frames_overridden = 0;  // frames actually replaced with user audio
  uint64_t resyncs = 0;            // position discontinuities (song change, seek, segment trim)
};

enum class LogLevel { kInfo, kWarning, kError };
using LogFn = std::function<void(LogLevel, const std::string&)>;

class Engine {
 public:
  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Loads <dir>/ipod.s3fp and <dir>/manifest.tsv. Returns false (and logs why) if the
  // folder is missing or invalid; the engine then stays inert.
  bool Load(const std::filesystem::path& dir, const LogFn& log);
  bool loaded() const { return loaded_.load(std::memory_order_acquire); }
  size_t num_songs() const { return song_samples_.size(); }
  size_t num_assigned() const;

  // Called once per decoded XMA frame, before the frame reaches guest memory.
  //   context_id   XMA context number
  //   packet       start of the 2048-byte XMA packet the frame belongs to
  //   frame_index  index of the frame within that packet (-1: frame split from previous)
  //   channels     1 or 2
  //   pcm_be       kSamplesPerFrame * channels interleaved big-endian int16 (in/out)
  // Returns true if pcm_be was replaced.
  bool OnFrame(uint32_t context_id, const uint8_t* packet, int frame_index, uint32_t channels,
               uint8_t* pcm_be);

  // Forget per-context position state (e.g. when the game resets a context).
  void ResetContext(uint32_t context_id);

  Stats stats() const;

 private:
  struct IndexEntry {
    uint64_t hash;
    uint32_t song;
    int32_t frame0_pos;  // song position of the first frame beginning in the packet
  };
  struct Slot;
  struct ContextState {
    int32_t song = -1;
    int64_t next_pos = 0;
    uint32_t unmatched_frames = 0;
  };

  const IndexEntry* Lookup(uint64_t hash) const;
  bool ReadPcm(Slot& slot, int64_t pos, int16_t* out_stereo);

  std::atomic<bool> loaded_{false};
  std::vector<IndexEntry> index_;
  std::vector<uint32_t> song_samples_;
  std::vector<std::unique_ptr<Slot>> slots_;  // by song index; null = not assigned
  std::unordered_map<uint32_t, ContextState> contexts_;
  Stats stats_;
  mutable std::mutex mutex_;
};

// Process-wide instance used by the XMA frame filter.
Engine& GlobalEngine();

}  // namespace skate3::custom_music
