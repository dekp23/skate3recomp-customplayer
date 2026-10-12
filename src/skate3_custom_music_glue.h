// Connects skate3::custom_music::Engine to rexglue's XMA frame filter.
#pragma once

#include <filesystem>

#include "skate3_custom_music_library.h"
#include "skate3_custom_music_metadata.h"

namespace skate3::custom_music {

// Sets up custom music: library folder (cvar skate3_custom_music_dir, default custom_music
// next to the executable; created if missing), fingerprint index and long-slot layout built
// from the game's files when needed, playlist player attached, XMA frame filter installed.
// No-op if disabled (cvar skate3_custom_music = false).
// Returns a host folder to mount over the guest data\audio\music folder (long slots
// active), or an empty path.
std::filesystem::path Install(const std::filesystem::path& game_data_root);

// Guest path the music overlay replaces.
constexpr const char* kGuestMusicPath = "\\Device\\Harddisk0\\Partition1\\data\\audio\\music";

// Converts a music file with the runtime's bundled FFmpeg (no external tools).
library::Converter MakeInGameConverter();

// HTTPS GET for song info lookups (WinHTTP; MusicBrainz User-Agent). -1 elsewhere/offline.
meta::HttpGet MakeHttpGet();

// Removes the filter and logs usage statistics.
void Uninstall();

}  // namespace skate3::custom_music
