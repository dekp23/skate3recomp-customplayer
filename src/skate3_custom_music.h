// Skate 3 custom music (dekp23): replaces in-game iPod songs with user audio at the XMA
// decoder output. Game files are never modified.
//
// How it works
//   The library folder (see skate3_custom_music_library.h) contains:
//     ipod.s3fp     fingerprints of every XMA packet in Ipod_Stream.mus, each mapped to
//                   (song slot, sample position within that song)
//     manifest.tsv  song slot -> user PCM file (44.1 kHz stereo s16le) + title
//     *.pcm         converted user tracks
//   The runtime calls Engine::OnFrame for every decoded XMA frame. If the frame's source
//   packet is an iPod packet, the 512 decoded samples are replaced with the user track at
//   the same position. Everything else (SFX, other music) passes through untouched.
//
// This file is pure C++17 with no rexglue dependency so it can be unit-tested standalone.
#pragma once

#include <atomic>
#include <chrono>
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
constexpr uint32_t kIndexVersion = 4;
constexpr double kSampleRate = 44100.0;

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

struct SlotInfo {
  uint32_t song = 0;
  double song_seconds = 0;   // length of the in-game song this slot replaces
  bool assigned = false;
  std::string title;         // user track title (empty if not assigned)
  double track_seconds = 0;  // user track length
};

struct NowPlaying {
  bool active = false;        // an iPod song was decoded within the recent window
  bool replaced = false;      // ... and it is playing user audio
  uint32_t song = 0;
  std::string title;
  double position_seconds = 0;
  double track_seconds = 0;   // user track length (0 if not replaced)
  double slot_seconds = 0;    // in-game slot length
  uint64_t serial = 0;        // increments every time a different song starts
};

// Diagnostic trace of iPod playback (Phase 0: how the game moves between songs).
struct TraceEvent {
  enum class Kind : uint8_t {
    kSegment,     // a context started decoding a segment
    kSongChange,  // a segment of a different song started (on any context)
    kReset,       // the game reset/released an XMA context
  };
  Kind kind = Kind::kSegment;
  double t_ms = 0;            // steady clock, ms since the engine was created
  uint32_t context = 0;
  int32_t song = -1;          // kSegment: song; kSongChange: new song
  int32_t segment = -1;       // kSegment: segment within the song
  int32_t chain_index = -1;   // kSegment: occurrence in the slot chain
  int64_t pos = 0;            // kSegment / kSongChange: slot position (samples)
  int64_t slot_length = 0;    // length of `song`'s slot (samples)
  int32_t from_song = -1;     // kSongChange
  int64_t from_pos = 0;       // kSongChange: last position decoded of from_song
  int64_t from_length = 0;    // kSongChange: from_song's slot length
  double gap_ms = 0;          // kSongChange: time since from_song's last frame
};
std::string FormatTrace(const TraceEvent& e);

// The game started (or restarted) an iPod song slot.
struct SlotEvent {
  int32_t song = -1;          // slot now playing
  int64_t pos = 0;            // its position (samples) at the first decoded frame
  int64_t length = 0;         // its length (samples, current chain)
  int32_t from_song = -1;     // slot played before (-1: none)
  int64_t from_pos = 0;       // last position decoded of from_song
  int64_t from_length = 0;
  bool restart = false;       // same slot started again from its beginning
};

// Supplies the audio for iPod frames instead of the per-slot manifest tracks
// (the playlist player). Called with the engine lock held, on the XMA worker thread:
// must not block.
class Renderer {
 public:
  virtual ~Renderer() = default;
  virtual void OnSlotStart(const SlotEvent& e) = 0;
  // Fills kSamplesPerFrame interleaved stereo samples for slot `song` at `pos`.
  // Returns false to leave the game's audio untouched.
  virtual bool Render(int32_t song, int64_t pos, int64_t slot_length, int16_t* stereo) = 0;
};

enum class LogLevel { kInfo, kWarning, kError };
using LogFn = std::function<void(LogLevel, const std::string&)>;

class Engine {
 public:
  using Clock = std::chrono::steady_clock;

  Engine();
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Loads <dir>/ipod.s3fp and <dir>/manifest.tsv. File work happens outside the audio
  // lock; the new state is swapped in atomically, so this is safe while music plays.
  // On failure the previous state (if any) is kept and false is returned.
  bool Load(const std::filesystem::path& dir, const LogFn& log);
  bool loaded() const { return loaded_.load(std::memory_order_acquire); }
  std::filesystem::path dir() const;
  size_t num_songs() const;
  size_t num_assigned() const;
  std::vector<uint32_t> song_samples() const;
  std::vector<SlotInfo> Slots() const;

  // Segment play order per song slot ("chain"), e.g. from a rewritten ipod.mpf: a slot
  // can repeat its own segments (longer) or stop early (shorter). Segment indices are
  // within the song. Empty or invalid chains fall back to the original order. Persists
  // across Load() calls.
  void SetChains(std::vector<std::vector<uint16_t>> chains);
  std::vector<std::vector<uint32_t>> segment_samples() const;
  std::vector<std::vector<uint16_t>> chains() const;

  // Runtime controls (thread-safe).
  void SetEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  void SetGain(float gain);  // linear, clamped to [0, 2]
  float gain() const { return gain_.load(std::memory_order_relaxed); }

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

  // What the iPod is playing, based on frames decoded within `window` before `now`.
  NowPlaying GetNowPlaying(Clock::time_point now,
                           Clock::duration window = std::chrono::milliseconds(1500)) const;

  Stats stats() const;

  // Non-owning; nullptr returns to per-slot manifest tracks. The renderer must outlive
  // its registration.
  void SetRenderer(Renderer* renderer);

  // Playback trace (bounded; oldest events dropped). Off by default.
  void SetTraceEnabled(bool on) { trace_enabled_.store(on, std::memory_order_relaxed); }
  std::vector<TraceEvent> DrainTrace();
  // Time base of TraceEvent::t_ms, for correlating other log lines (e.g. F10 markers).
  double TraceNowMs() const {
    return std::chrono::duration<double, std::milli>(Clock::now() - epoch_).count();
  }

 private:
  void PushTrace(TraceEvent e);
  struct IndexEntry {
    uint64_t hash;
    uint16_t song;
    uint16_t segment;  // within the song
    int32_t offset;    // first frame of the packet, relative to its segment's start
  };
  struct Slot;
  struct ContextState {
    int32_t song = -1;
    int32_t chain_index = -1;  // occurrence of the current segment in the song's chain
    int64_t next_pos = 0;
    uint32_t unmatched_frames = 0;
  };
  struct SongLayout {
    std::vector<uint32_t> segment_samples;   // original segments of the song
    std::vector<uint16_t> chain;             // play order (segment indices)
    std::vector<int64_t> timeline;           // start of each chain entry in the slot
    int64_t length = 0;                      // total slot length in samples
    int32_t last_started = -1;               // most recently started chain entry
    int64_t last_pos = 0;                    // last decoded position
    Clock::time_point last_time{};           // when it was decoded
  };

  const IndexEntry* Lookup(uint64_t hash) const;
  bool ReadPcm(Slot& slot, int64_t pos, int16_t* out_stereo);
  int32_t ResolveChainIndex(ContextState& st, SongLayout& song, uint16_t segment);
  static void ApplyChain(SongLayout& song, const std::vector<uint16_t>* chain);

  std::atomic<bool> loaded_{false};
  std::atomic<bool> enabled_{true};
  std::atomic<float> gain_{1.0f};
  std::filesystem::path dir_;
  std::vector<IndexEntry> index_;
  std::vector<SongLayout> songs_;
  std::vector<std::vector<uint16_t>> chains_override_;
  std::vector<std::unique_ptr<Slot>> slots_;  // by song index; null = not assigned
  std::unordered_map<uint32_t, ContextState> contexts_;
  Stats stats_;
  // Now-playing tracking.
  int32_t last_song_ = -1;
  int64_t last_pos_ = 0;
  Clock::time_point last_time_{};
  uint64_t serial_ = 0;
  // Slot tracking (segment starts on any context).
  int32_t slot_song_ = -1;
  int32_t prev_slot_song_ = -1;
  Clock::time_point slot_change_time_{};
  Renderer* renderer_ = nullptr;
  // Trace.
  std::atomic<bool> trace_enabled_{false};
  std::vector<TraceEvent> trace_;
  uint64_t trace_dropped_ = 0;
  Clock::time_point epoch_ = Clock::now();
  mutable std::mutex mutex_;
};

// Process-wide instance used by the XMA frame filter and the UI.
Engine& GlobalEngine();

}  // namespace skate3::custom_music
