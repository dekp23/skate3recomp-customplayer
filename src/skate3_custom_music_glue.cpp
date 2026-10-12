#include "skate3_custom_music_glue.h"

#include "skate3_custom_music.h"
#include "skate3_custom_music_library.h"
#include "skate3_custom_music_mpf.h"
#include "skate3_custom_music_mus.h"
#include "skate3_custom_music_player.h"
#include "skate3_custom_music_ui.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

#include <rex/audio/xma/frame_filter.h>
#include <rex/audio/xma/stream_decoder.h>
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

// Diagnostic: decodes the first iPod segment with the in-process XMA stream decoder and
// logs the result (proves the runtime's decoder is reachable from the game module).
void DecoderSelfTest(const std::filesystem::path& game_music) {
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<mus::Segment> segs;
  std::string err;
  const auto mus_file = game_music / "Ipod_Stream.mus";
  if (!mus::ReadTable(mus_file, &segs, &err) || segs.empty()) {
    REXLOG_WARN("custom music: decoder self-test skipped: {}", err);
    return;
  }
  std::ifstream in(mus_file, std::ios::binary);
  std::vector<mus::Block> blocks;
  if (!mus::ReadBlocks(in, segs[0], &blocks, &err)) {
    REXLOG_WARN("custom music: decoder self-test: {}", err);
    return;
  }
  const auto padded = mus::PadPackets(blocks);
  rex::audio::XmaStreamDecoder dec;
  if (!dec.Open(segs[0].sample_rate, segs[0].channels)) {
    REXLOG_WARN("custom music: decoder self-test: XMA decoder unavailable");
    return;
  }
  std::vector<int16_t> pcm;
  const int frames = dec.DecodeStream(padded.data(), blocks.size(), &pcm);
  double sum = 0;
  for (int16_t v : pcm) sum += double(v) * v;
  const double rms = pcm.empty() ? 0 : std::sqrt(sum / pcm.size());
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  REXLOG_INFO("custom music: decoder self-test: segment 0 -> {} frames ({} expected), "
              "rms {:.0f}, {:.1f} ms",
              frames, (segs[0].num_samples + mus::LeadSamples(blocks) + 511) / 512, rms, ms);
}
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
  REXLOG_INFO("custom music: long iPod slots active ({} nodes)", graph.num_nodes);
  return active;
}

// Decodes one of the game's own iPod songs from Ipod_Stream.mus with the runtime's XMA
// decoder (worker thread of the player).
Player::OriginalDecoder MakeOriginalDecoder(const std::filesystem::path& game_music) {
  struct Shared {
    std::filesystem::path mus_file;
    std::vector<mus::Segment> segments;
    std::vector<uint32_t> first;
    std::vector<std::vector<uint32_t>> songs;
  };
  auto shared = std::make_shared<Shared>();
  shared->mus_file = game_music / "Ipod_Stream.mus";
  std::string err;
  if (!mus::ReadTable(shared->mus_file, &shared->segments, &err)) {
    REXLOG_WARN("custom music: cannot read the game's iPod songs: {}", err);
    return {};
  }
  shared->songs = mus::GroupSongs(shared->segments, &shared->first);
  return [shared](uint32_t song, std::vector<int16_t>* pcm) {
    if (song >= shared->songs.size()) return false;
    const auto t0 = std::chrono::steady_clock::now();
    std::ifstream in(shared->mus_file, std::ios::binary);
    const auto& seg0 = shared->segments[shared->first[song]];
    rex::audio::XmaStreamDecoder dec;
    if (!dec.Open(seg0.sample_rate, seg0.channels)) return false;
    std::vector<std::vector<int16_t>> decoded;
    std::vector<int32_t> lead;
    std::vector<uint32_t> counts;
    std::vector<mus::Block> blocks;
    std::string e;
    for (size_t k = 0; k < shared->songs[song].size(); ++k) {
      const auto& seg = shared->segments[shared->first[song] + k];
      if (!mus::ReadBlocks(in, seg, &blocks, &e)) {
        REXLOG_WARN("custom music: song {} segment {}: {}", song, k, e);
        return false;
      }
      const auto packets = mus::PadPackets(blocks);
      std::vector<int16_t> out;
      if (dec.DecodeStream(packets.data(), blocks.size(), &out) <= 0) {
        REXLOG_WARN("custom music: song {} segment {}: XMA decode failed", song, k);
        return false;
      }
      decoded.push_back(std::move(out));
      lead.push_back(mus::LeadSamples(blocks));
      counts.push_back(seg.num_samples);
    }
    auto joined = mus::JoinSegments(decoded, lead, counts, seg0.channels);
    if (seg0.channels == 1) {
      pcm->resize(joined.size() * 2);
      for (size_t i = 0; i < joined.size(); ++i) (*pcm)[2 * i] = (*pcm)[2 * i + 1] = joined[i];
    } else {
      *pcm = std::move(joined);
    }
    REXLOG_INFO("custom music: decoded game song {} ({:.0f} s of audio) in {:.0f} ms", song + 1,
                pcm->size() / 2 / 44100.0,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return true;
  };
}

}  // namespace

std::filesystem::path Install(const std::filesystem::path& game_data_root) {
  if (!REXCVAR_GET(skate3_custom_music)) {
    REXLOG_INFO("custom music: disabled by skate3_custom_music=false");
    return {};
  }
  const auto dir = ResolveLibraryDir(game_data_root);
  const auto game_music = game_data_root / "data" / "audio" / "music";
  {
    std::string msg;
    const bool had_index = !library::ReadIndexSongSamples(dir / "ipod.s3fp").empty();
    if (!library::EnsureIndex(dir, game_music, &msg)) {
      REXLOG_WARN("custom music: {}", msg);
    } else if (!had_index) {
      REXLOG_INFO("custom music: {}", msg);
    }
  }
  {
    bool changed = false;
    std::string msg;
    if (!library::EnsureCarrierLayout(dir, game_music, &changed, &msg)) {
      REXLOG_WARN("custom music: {}", msg);
    } else if (changed) {
      REXLOG_INFO("custom music: {}", msg);
    }
  }
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
  // Playlist player: the game's songs + your tracks, decides what every iPod frame plays.
  GlobalPlayer().SetOriginalDecoder(MakeOriginalDecoder(game_music));
  ReloadPlaylist(dir);
  GlobalEngine().SetRenderer(&GlobalPlayer());
  {
    const auto pl = GlobalPlayer().playlist();
    size_t on = 0;
    for (const auto& e : pl) on += e.enabled;
    REXLOG_INFO("custom music: playlist {} entries ({} on), shuffle {}", pl.size(), on,
                GlobalPlayer().Status().shuffle ? "on" : "off");
  }
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
    std::thread([game_music] { DecoderSelfTest(game_music); }).detach();
  }
  g_installed = true;
  return overlay;
}

void Uninstall() {
  if (!g_installed.exchange(false)) return;
  rex::audio::SetXmaFrameFilter(nullptr);
  GlobalEngine().SetRenderer(nullptr);
  GlobalEngine().SetTraceEnabled(false);
  g_trace_logger.Stop();
  const Stats st = GlobalEngine().stats();
  REXLOG_INFO(
      "custom music stats: frames seen={} matched={} bridged={} replaced={} resyncs={}",
      st.frames_seen, st.frames_matched, st.frames_continued, st.frames_overridden, st.resyncs);
}

}  // namespace skate3::custom_music
