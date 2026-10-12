#include "skate3_custom_music_glue.h"

#include "skate3_custom_music.h"
#include "skate3_custom_music_library.h"
#include "skate3_custom_music_mpf.h"
#include "skate3_custom_music_ui.h"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include <rex/audio/xma/frame_filter.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(skate3_custom_music, true, "Skate 3",
                    "Replace in-game iPod songs with your own music prepared by "
                    "tools/skate3_music_import (no effect if the folder is missing)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(skate3_custom_music_dir, "", "Skate 3",
                      "Custom music folder (default: custom_music next to the executable, "
                      "else <game>/data/audio/music/custom)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(skate3_custom_music_trace, true, "Skate 3",
                    "Log iPod playback events (MUSICTRACE lines: segment starts, song "
                    "changes, context resets) for diagnosing song transitions")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace skate3::custom_music {
namespace {

std::atomic<bool> g_installed{false};

// Writes the engine's playback trace to the log from a background thread, so the audio
// thread never does log I/O.
class TraceLogger {
 public:
  void Start() {
    if (thread_.joinable()) return;
    stop_ = false;
    thread_ = std::thread([this] {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!stop_) {
        cv_.wait_for(lock, std::chrono::milliseconds(250));
        lock.unlock();
        Flush();
        lock.lock();
      }
    });
  }
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    Flush();
  }

 private:
  static void Flush() {
    for (const auto& e : GlobalEngine().DrainTrace()) REXLOG_INFO("{}", FormatTrace(e));
  }
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
};

TraceLogger g_trace_logger;
std::atomic<bool> g_first_override_logged{false};

bool OnFrame(void*, uint32_t context_id, const uint8_t* packet, int frame_index,
             uint32_t channels, uint8_t* pcm_be) {
  const bool replaced =
      GlobalEngine().OnFrame(context_id, packet, frame_index, channels, pcm_be);
  if (replaced && !g_first_override_logged.exchange(true)) {
    REXLOG_INFO("custom music: first iPod frame replaced (XMA context {})", context_id);
  }
  return replaced;
}

void OnContextReset(void*, uint32_t context_id) { GlobalEngine().ResetContext(context_id); }

}  // namespace

std::filesystem::path ResolveLibraryDir(const std::filesystem::path& game_data_root) {
  // Lookup order: explicit cvar, then "custom_music" next to the executable (portable,
  // independent of settings.toml, which the settings menu rewrites with its own keys
  // only), then the game data folder.
  const std::string configured = REXCVAR_GET(skate3_custom_music_dir);
  if (!configured.empty()) return PathFromUtf8(configured);
  std::error_code ec;
  const auto beside_exe = rex::filesystem::GetAppRootFolder() / "custom_music";
  if (std::filesystem::is_directory(beside_exe, ec)) return beside_exe;
  const auto in_game = game_data_root / "data" / "audio" / "music" / "custom";
  if (std::filesystem::is_directory(in_game, ec)) return in_game;
  return beside_exe;  // where the in-game menu will create one
}

namespace {

// Builds <library>/game_music: hard links to the game's music files plus the song-length
// layout as ipod.mpf. Returns that folder, or empty if no layout applies.
std::filesystem::path PrepareMusicOverlay(const std::filesystem::path& library,
                                          const std::filesystem::path& game_music) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path layout = library / library::kLayoutFileName;
  if (!fs::is_regular_file(layout, ec) || !fs::is_directory(game_music, ec)) return {};

  // Validate the layout and read its chains before touching anything.
  std::ifstream lf(layout, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(lf)), {});
  const auto segments = library::ReadIndexSegments(library / "ipod.s3fp");
  std::vector<uint32_t> per_song;
  for (const auto& sg : segments) per_song.push_back(static_cast<uint32_t>(sg.size()));
  mpf::Graph graph;
  std::vector<mpf::SongChain> chains;
  std::string err;
  if (segments.empty() || !mpf::Parse(bytes, &graph, &err) ||
      !mpf::ReadChains(graph, per_song, &chains, &err)) {
    REXLOG_WARN("custom music: ignoring {}: {}", PathToUtf8(layout), err);
    return {};
  }

  const fs::path active = library / "game_music";
  fs::create_directories(active, ec);
  for (const auto& entry : fs::directory_iterator(game_music, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const fs::path name = entry.path().filename();
    if (name == "ipod.mpf") continue;
    const fs::path dst = active / name;
    // Hard links share size and timestamps with the original; relink when they differ.
    std::error_code e1, e2;
    if (fs::exists(dst, e1) && fs::file_size(dst, e1) == fs::file_size(entry.path(), e2) &&
        fs::last_write_time(dst, e1) == fs::last_write_time(entry.path(), e2)) {
      continue;
    }
    fs::remove(dst, e1);
    fs::create_hard_link(entry.path(), dst, e1);
    if (e1) {
      REXLOG_WARN("custom music: cannot hard-link {} ({}); song lengths stay original. "
                  "The library and the game must be on the same drive.",
                  PathToUtf8(name), e1.message());
      return {};
    }
  }
  fs::copy_file(layout, active / "ipod.mpf", fs::copy_options::overwrite_existing, ec);
  if (ec) {
    REXLOG_WARN("custom music: cannot stage ipod.mpf: {}", ec.message());
    return {};
  }
  std::vector<std::vector<uint16_t>> chain_list;
  for (const auto& c : chains) chain_list.push_back(c.segments);
  GlobalEngine().SetChains(std::move(chain_list));
  REXLOG_INFO("custom music: song-length layout active ({} nodes)", graph.num_nodes);
  return active;
}

}  // namespace

std::filesystem::path Install(const std::filesystem::path& game_data_root) {
  if (!REXCVAR_GET(skate3_custom_music)) {
    REXLOG_INFO("custom music: disabled by skate3_custom_music=false");
    return {};
  }
  const auto dir = ResolveLibraryDir(game_data_root);
  const auto game_music = game_data_root / "data" / "audio" / "music";
  const auto settings = PlayerSettings::Load(dir);
  settings.ApplyTo(GlobalEngine());
  GlobalEngine().Load(dir, [](LogLevel level, const std::string& msg) {
    switch (level) {
      case LogLevel::kInfo:
        REXLOG_INFO("{}", msg);
        break;
      case LogLevel::kWarning:
        REXLOG_WARN("{}", msg);
        break;
      case LogLevel::kError:
        REXLOG_ERROR("{}", msg);
        break;
    }
  });
  const auto overlay = PrepareMusicOverlay(dir, game_music);
  {
    std::lock_guard<std::mutex> lock(GlobalUiState().mutex);
    GlobalUiState().library_dir = dir;
    GlobalUiState().game_music_dir = game_music;
    GlobalUiState().layout_active = !overlay.empty();
    GlobalUiState().settings = settings;
  }
  REXLOG_INFO("custom music (dekp23): library {}, F7 opens the music menu", PathToUtf8(dir));

  // Installed even without a loaded library: the F7 menu can build/load one later.
  rex::audio::XmaFrameFilter filter;
  filter.on_frame = &OnFrame;
  filter.on_context_reset = &OnContextReset;
  rex::audio::SetXmaFrameFilter(&filter);
  if (REXCVAR_GET(skate3_custom_music_trace)) {
    GlobalEngine().SetTraceEnabled(true);
    g_trace_logger.Start();
    REXLOG_INFO("custom music: playback trace on (MUSICTRACE lines; F10 writes a marker)");
  }
  g_installed = true;
  return overlay;
}

void Uninstall() {
  if (!g_installed.exchange(false)) return;
  rex::audio::SetXmaFrameFilter(nullptr);
  GlobalEngine().SetTraceEnabled(false);
  g_trace_logger.Stop();
  const Stats st = GlobalEngine().stats();
  REXLOG_INFO(
      "custom music stats: frames seen={} matched={} bridged={} replaced={} resyncs={}",
      st.frames_seen, st.frames_matched, st.frames_continued, st.frames_overridden, st.resyncs);
}

}  // namespace skate3::custom_music
