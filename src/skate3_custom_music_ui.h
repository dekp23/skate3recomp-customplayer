// Skate 3 custom music (dekp23): in-game UI.
//   NowPlayingToast  - input-transparent "Now playing" pop-up when one of your tracks starts
//   CustomMusicMenu  - F7 window: on/off, volume, add/import tracks, rebuild, slot table
#pragma once

#include <atomic>
#include <functional>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rex/ui/imgui_dialog.h>

#include "skate3_custom_music.h"

namespace skate3::custom_music {

// Persisted in <library>/player.ini (settings.toml is rewritten by the settings menu).
struct PlayerSettings {
  bool enabled = true;
  bool toasts = true;
  bool fill_repeat = true;
  int volume_percent = 100;

  static PlayerSettings Load(const std::filesystem::path& library_dir);
  bool Save(const std::filesystem::path& library_dir) const;
  void ApplyTo(Engine& engine) const;
};

// Shared between the toast and the menu.
struct UiState {
  std::filesystem::path library_dir;
  std::filesystem::path game_music_dir;  // original game music folder (read only)
  bool layout_active = false;            // a song-length layout was mounted at startup
  PlayerSettings settings;
  std::mutex mutex;
};
UiState& GlobalUiState();

class NowPlayingToast final : public rex::ui::ImGuiDialog {
 public:
  explicit NowPlayingToast(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  bool WantsContinuousRepaint() const override { return false; }
  void OnDraw(ImGuiIO& io) override;

 private:
  uint64_t shown_serial_ = 0;
  Engine::Clock::time_point show_until_{};
  Engine::Clock::time_point shown_at_{};
};

class CustomMusicMenu final : public rex::ui::ImGuiDialog {
 public:
  explicit CustomMusicMenu(rex::ui::ImGuiDrawer* drawer);
  ~CustomMusicMenu();

  void Show();
  void Hide();
  bool visible() const { return visible_; }
  // Called whenever the window closes (F7 or its close button).
  void set_on_hidden(std::function<void()> fn) { on_hidden_ = std::move(fn); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class Job { kRebuild, kAddFiles, kImportMusicFolder };
  void StartJob(Job job);
  void RunJob(Job job);
  void SetStatus(const std::string& s, bool error = false);
  bool JobRunning() const { return job_running_.load(); }

  bool visible_ = false;
  std::function<void()> on_hidden_;
  std::thread worker_;
  std::atomic<bool> job_running_{false};
  std::atomic<bool> cancel_{false};
  std::mutex job_mutex_;
  std::string job_status_;
  bool job_error_ = false;
  float job_progress_ = -1.0f;
  bool restart_needed_ = false;
};

}  // namespace skate3::custom_music
