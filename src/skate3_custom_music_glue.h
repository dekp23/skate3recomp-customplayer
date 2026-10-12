// Connects skate3::custom_music::Engine to rexglue's XMA frame filter.
#pragma once

#include <filesystem>

namespace skate3::custom_music {

// Loads the custom music folder (cvar skate3_custom_music_dir, default
// <game_data_root>/data/audio/music/custom) and installs the XMA frame filter.
// No-op if disabled (cvar skate3_custom_music = false) or the folder is absent.
// Returns a host folder to mount over the guest data\audio\music folder (song-length
// layout active), or an empty path.
std::filesystem::path Install(const std::filesystem::path& game_data_root);

// Guest path the music overlay replaces.
constexpr const char* kGuestMusicPath = "\\Device\\Harddisk0\\Partition1\\data\\audio\\music";

// Removes the filter and logs usage statistics.
void Uninstall();

}  // namespace skate3::custom_music
