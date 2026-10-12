// Skate 3 custom music (dekp23): building the music library from the runtime.
//
// Library folder layout (same as tools/skate3_music_import `prepare`):
//   ipod.s3fp      fingerprint index (built once by the Python tool; never changes)
//   tracks.tsv     your converted tracks: PCM file, samples, title, source file
//   playlist.tsv   playlist order / on-off (written by the player menu)
//   ipod_layout.mpf  lengthened iPod slots, served to the game as ipod.mpf
//   manifest.tsv   legacy: slot -> PCM file, samples, title
//   *.pcm          44.1 kHz stereo s16le
//   tracks/        your source audio files (any format ffmpeg reads)
//
// Conversion runs an external ffmpeg (rexglue's bundled FFmpeg only decodes XMA/WMA).
// Pure C++17 apart from process spawning; unit-tested standalone.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace skate3::custom_music::library {

constexpr const char* kTracksDirName = "tracks";

// Makes sure <library>/ipod.s3fp exists and is valid; builds it from the game's
// <game_music_dir>/Ipod_Stream.mus if not (about a second). Creates the library folder.
// Returns false with *message set if no index could be made.
bool EnsureIndex(const std::filesystem::path& library_dir,
                 const std::filesystem::path& game_music_dir, std::string* message);

// Tags and cover read while converting a track.
struct TrackTags {
  std::string title, artist, album;
  std::vector<uint8_t> cover;  // embedded picture as stored (JPEG/PNG)
};
// Converts src into 44.1 kHz stereo s16le at dst_pcm; returns sample frames or -1.
using Converter = std::function<int64_t(const std::filesystem::path& src,
                                        const std::filesystem::path& dst_pcm, TrackTags* tags,
                                        std::string* error)>;

struct Options {
  // In-process converter (the runtime's FFmpeg). If empty, an installed ffmpeg is used.
  Converter converter;
  std::filesystem::path ffmpeg;
  bool fill_repeat = true;  // reuse tracks for leftover slots; false keeps the game's songs
  // Song-length fitting: the original game music folder (read only, for ipod.mpf). When
  // set, slots are lengthened/shortened to each track and <library>/ipod_layout.mpf is
  // written; it takes effect the next time the game starts.
  std::filesystem::path game_music_dir;
  // Legacy per-slot mode: fit slot lengths to the tracks and write ipod_layout.mpf. The
  // playlist player uses EnsureCarrierLayout() instead.
  bool fit_layout = false;
};

// Playlist mode: every iPod slot is lengthened to at least the same duration (as long as
// the node budget allows, ~20 minutes), so the game rarely changes song by itself; the
// player decides what is heard. Returns per-song chains.
std::vector<std::vector<uint16_t>> CarrierChains(
    const std::vector<std::vector<uint32_t>>& segments_by_song, size_t max_extra_nodes,
    int64_t* min_slot_samples = nullptr);
// Writes <library>/ipod_layout.mpf with carrier chains if it is missing or different.
// Needs ipod.s3fp and the game's ipod.mpf. *changed tells whether the file was written.
bool EnsureCarrierLayout(const std::filesystem::path& library_dir,
                         const std::filesystem::path& game_music_dir, bool* changed,
                         std::string* message);

constexpr const char* kLayoutFileName = "ipod_layout.mpf";
// Rewritten ipod.mpf node budget: node offsets are 16-bit word offsets (256 KiB).
constexpr size_t kMaxExtraNodes = 8000;

struct FlexibleAssignment {
  std::vector<int> track_for_slot;               // -1 = original game song
  std::vector<std::vector<uint16_t>> chains;     // empty = original chain
};

// Play order covering n segments of a song with `segments` segments: 0..n-1, extended by
// repeating segments 1..segments-1 (segment 0 is never repeated, so a restart of the song
// stays unambiguous at runtime).
std::vector<uint16_t> ChainOfLength(size_t segments, size_t n);
// Length-fitting assignment: longest tracks first, each to the free slot whose segment
// size leaves the least silence after the track; leftover slots per fill_repeat.
FlexibleAssignment AssignFlexible(const std::vector<std::vector<uint32_t>>& segments_by_song,
                                  const std::vector<int64_t>& track_samples, bool fill_repeat,
                                  size_t max_extra_nodes = kMaxExtraNodes);

struct BuildResult {
  bool ok = false;
  bool layout_written = false;                 // ipod_layout.mpf (re)written
  std::vector<std::vector<uint16_t>> chains;   // per slot, as written (empty = original)
  std::string message;
  size_t tracks = 0;
  size_t converted = 0;
  size_t reused = 0;
  size_t failed = 0;
  size_t slots_assigned = 0;
  std::vector<std::string> errors;
};

using ProgressFn = std::function<void(size_t done, size_t total, const std::string& current)>;

bool IsAudioFile(const std::filesystem::path& p);
// Recursive, sorted by path. Missing folder -> empty.
std::vector<std::filesystem::path> ScanFolder(const std::filesystem::path& folder);
// File stem without a trailing yt-dlp " [videoid]" tag.
std::string CleanTitle(const std::filesystem::path& file);
// Explicit path if it exists, else PATH and the usual winget locations. Empty if none.
std::filesystem::path FindFfmpeg(const std::filesystem::path& configured);
// Runs ffmpeg; returns stereo sample frames written, or -1 (error filled).
int64_t ConvertToPcm(const std::filesystem::path& ffmpeg, const std::filesystem::path& src,
                     const std::filesystem::path& dst, std::string* error);
// Best-fit assignment, identical to tools/skate3_music_import/s3mus/prepare.py assign().
// Returns, per slot, the index into track_samples or -1.
std::vector<int> Assign(const std::vector<uint32_t>& song_samples,
                        const std::vector<int64_t>& track_samples, bool fill_repeat);
// Per-song segment lengths from an ipod.s3fp index (v4). Empty on error.
std::vector<std::vector<uint32_t>> ReadIndexSegments(const std::filesystem::path& index_file);
// Song lengths from an ipod.s3fp index. Empty on error.
std::vector<uint32_t> ReadIndexSongSamples(const std::filesystem::path& index_file);
// Copies files into tracks_dir (existing names are skipped). Returns files copied.
size_t ImportFiles(const std::vector<std::filesystem::path>& files,
                   const std::filesystem::path& tracks_dir, std::vector<std::string>* errors);
// Converts tracks_dir into PCM (cached by path/size/mtime), assigns slots and writes
// manifest.tsv atomically. Does not touch the running engine; call Engine::Load after.
BuildResult Rebuild(const std::filesystem::path& library_dir, const Options& options,
                    const ProgressFn& progress, const std::atomic<bool>* cancel);
// Deletes *.pcm files that manifest.tsv no longer references. Returns files removed.
size_t RemoveUnusedPcm(const std::filesystem::path& library_dir);

}  // namespace skate3::custom_music::library
