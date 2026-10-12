// Skate 3 custom music (dekp23): playlist player ("radio mode").
//
// The game keeps playing its iPod song slots; this player decides what is heard. The
// position inside the current game slot is the clock: an anchor maps a slot position to
// (playlist entry, offset), and every frame is located by walking the playlist from its
// anchor. The mapping is a pure function of the slot position, so the game decoding the
// same slot on several XMA contexts at once (prefetch) still gets exact audio.
//
//   slot ends by itself (game reached the end of its long carrier slot) -> continue
//   game slot changes early (song picked in the game's music player)     -> next entry
//   Next / Previous from the menu                                         -> re-anchor
//
// Sources: the game's own iPod songs (decoded from Ipod_Stream.mus by an injected
// decoder) and your tracks (44.1 kHz stereo s16le PCM files). Decoding runs on a worker
// thread; Render() only reads memory. Pure C++17 (unit-tested standalone).
#pragma once

#include "skate3_custom_music.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace skate3::custom_music {

struct PlaylistEntry {
  enum class Kind { kOriginal, kTrack };
  Kind kind = Kind::kOriginal;
  uint32_t original = 0;               // kOriginal: iPod song index
  std::filesystem::path pcm;           // kTrack: PCM file
  int64_t samples = 0;                 // length (stereo frames)
  std::string title, artist, album;
  bool enabled = true;
  // Stable identity for saving: "orig:17" or "track:<pcm file name>".
  std::string key() const;
};

struct PlayerStatus {
  bool active = false;          // iPod audio decoded recently
  int entry = -1;               // index into the playlist
  std::string title, artist;
  double position_seconds = 0, length_seconds = 0;
  uint64_t serial = 0;          // changes when a different entry starts
  bool shuffle = false;
};

class Player : public Renderer {
 public:
  using Clock = std::chrono::steady_clock;
  // Decodes a whole original song into interleaved stereo s16 PCM. Thread: worker.
  using OriginalDecoder = std::function<bool(uint32_t song, std::vector<int16_t>* pcm)>;

  Player();
  ~Player() override;

  void SetOriginalDecoder(OriginalDecoder decoder);
  void SetPlaylist(std::vector<PlaylistEntry> entries);
  std::vector<PlaylistEntry> playlist() const;
  void SetShuffle(bool on);
  void SetEnabled(size_t entry, bool enabled);
  void Move(size_t entry, int delta);  // reorder (playlist order, not shuffle order)
  void Next();
  void Previous();
  void PlayEntry(size_t entry);
  PlayerStatus Status(Clock::time_point now = Clock::now()) const;

  // Renderer (engine lock held, XMA worker thread).
  void OnSlotStart(const SlotEvent& e) override;
  bool Render(int32_t song, int64_t pos, int64_t slot_length, int16_t* stereo) override;

  // Test hooks.
  void WaitIdle();  // blocks until the worker has nothing queued
  static constexpr int64_t kNaturalEndWindow = 20 * 44100;  // slot "ended by itself"

 private:
  struct Anchor {
    int64_t slot_pos = 0;   // slot position the anchor starts at
    size_t order_pos = 0;   // index into order_
    int64_t offset = 0;     // offset into that entry at slot_pos
  };
  struct Located {
    bool ok = false;
    size_t order_pos = 0;
    int64_t offset = 0;
  };
  struct Audio;  // decoded / opened source

  Located Locate(int32_t song, int64_t pos) const;           // mutex_ held
  void Reanchor(int32_t song, int64_t slot_pos, size_t order_pos, int64_t offset);
  void RebuildOrder(int keep_entry, bool reshuffle);          // mutex_ held
  size_t Wrap(int64_t order_pos) const;
  int64_t EntryLength(size_t order_pos) const;
  bool ReadSamples(size_t entry, int64_t offset, int16_t* stereo);  // mutex_ held
  void Want(size_t entry);                                    // mutex_ held
  void WorkerLoop();

  mutable std::mutex mutex_;
  std::vector<PlaylistEntry> entries_;
  std::vector<size_t> order_;             // enabled entries in play order
  bool shuffle_ = false;
  std::mt19937 rng_{std::random_device{}()};
  std::map<int32_t, std::vector<Anchor>> anchors_;  // per slot, ascending slot_pos
  int32_t current_song_ = -1;             // slot most recently started
  // Last rendered frame (for continuation, status, next/previous).
  int32_t last_song_ = -1;
  int64_t last_pos_ = 0;
  size_t last_order_pos_ = 0;
  int64_t last_offset_ = 0;
  Clock::time_point last_time_{};
  int last_entry_ = -1;
  uint64_t serial_ = 0;
  size_t start_order_pos_ = 0;            // where the first slot starts

  // Sources: entry index -> audio. Worker loads, Render reads.
  std::map<size_t, std::shared_ptr<Audio>> audio_;
  std::vector<size_t> wanted_;
  uint64_t version_ = 0;  // bumped when entry indices change
  OriginalDecoder decoder_;
  std::condition_variable cv_;
  bool busy_ = false;
  bool stop_ = false;
  std::thread worker_;
};

// Default playlist: the 46 originals then your tracks (from tracks.tsv), with the saved
// order/enabled state from playlist.tsv applied. Titles: tracks.tsv / "Skate 3 song N".
std::vector<PlaylistEntry> LoadPlaylist(const std::filesystem::path& library_dir,
                                        const std::vector<uint32_t>& original_samples);
bool SavePlaylist(const std::filesystem::path& library_dir,
                  const std::vector<PlaylistEntry>& entries, bool shuffle);
bool LoadShuffle(const std::filesystem::path& library_dir);

// Process-wide player used by the engine, the toast and the menu.
Player& GlobalPlayer();

}  // namespace skate3::custom_music
