// Skate 3 custom music (dekp23): building the music library from the runtime.
//
// Library folder layout (same as tools/skate3_music_import `prepare`):
//   ipod.s3fp      fingerprint index (built once by the Python tool; never changes)
//   manifest.tsv   slot -> PCM file, samples, title
//   *.pcm          44.1 kHz stereo s16le
//   tracks/        your source audio files (any format ffmpeg reads)
//
// Conversion runs an external ffmpeg (rexglue's bundled FFmpeg only decodes XMA/WMA).
// Pure C++17 apart from process spawning; unit-tested standalone.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace skate3::custom_music::library {

constexpr const char* kTracksDirName = "tracks";

struct Options {
  std::filesystem::path ffmpeg;
  bool fill_repeat = true;  // reuse tracks for leftover slots; false keeps the game's songs
};

struct BuildResult {
  bool ok = false;
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
