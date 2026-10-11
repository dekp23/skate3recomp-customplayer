#include "skate3_custom_music_glue.h"

#include "skate3_custom_music.h"

#include <atomic>
#include <string>

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

namespace skate3::custom_music {
namespace {

std::atomic<bool> g_installed{false};
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

void Install(const std::filesystem::path& game_data_root) {
  if (!REXCVAR_GET(skate3_custom_music)) {
    REXLOG_INFO("custom music: disabled by skate3_custom_music=false");
    return;
  }
  // Lookup order: explicit cvar, then "custom_music" next to the executable (portable,
  // independent of settings.toml, which the settings menu rewrites with its own keys
  // only), then the game data folder.
  std::filesystem::path dir;
  const std::string configured = REXCVAR_GET(skate3_custom_music_dir);
  std::error_code ec;
  const auto beside_exe = rex::filesystem::GetAppRootFolder() / "custom_music";
  if (!configured.empty()) {
    dir = PathFromUtf8(configured);
  } else if (std::filesystem::is_directory(beside_exe, ec)) {
    dir = beside_exe;
  } else {
    dir = game_data_root / "data" / "audio" / "music" / "custom";
  }

  const bool ok = GlobalEngine().Load(dir, [](LogLevel level, const std::string& msg) {
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
  if (!ok) return;

  rex::audio::XmaFrameFilter filter;
  filter.on_frame = &OnFrame;
  filter.on_context_reset = &OnContextReset;
  rex::audio::SetXmaFrameFilter(&filter);
  g_installed = true;
}

void Uninstall() {
  if (!g_installed.exchange(false)) return;
  rex::audio::SetXmaFrameFilter(nullptr);
  const Stats st = GlobalEngine().stats();
  REXLOG_INFO(
      "custom music stats: frames seen={} matched={} bridged={} replaced={} resyncs={}",
      st.frames_seen, st.frames_matched, st.frames_continued, st.frames_overridden, st.resyncs);
}

}  // namespace skate3::custom_music
