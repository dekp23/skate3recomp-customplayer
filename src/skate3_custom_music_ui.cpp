#include "skate3_custom_music_ui.h"

#include "skate3_custom_music_library.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

#include <imgui.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace skate3::custom_music {
namespace fs = std::filesystem;

namespace {

// Branding (dekp23).
constexpr const char* kBrand = "dekp23";
constexpr const char* kBrandUrl = "github.com/dekp23";
const ImVec4 kAccent(1.00f, 0.36f, 0.21f, 1.0f);     // skate orange
const ImVec4 kAccentDim(1.00f, 0.36f, 0.21f, 0.55f);
const ImVec4 kMuted(0.70f, 0.72f, 0.76f, 1.0f);
const ImVec4 kGood(0.40f, 0.90f, 0.50f, 1.0f);
const ImVec4 kWarn(1.00f, 0.75f, 0.30f, 1.0f);
const ImVec4 kBad(1.00f, 0.42f, 0.42f, 1.0f);

std::string FormatTime(double seconds) {
  if (seconds < 0) seconds = 0;
  const int s = static_cast<int>(seconds + 0.5);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
  return buf;
}

void PushFontOr(ImFont* font, float size) { ImGui::PushFont(font, size); }

#if defined(_WIN32)
std::vector<fs::path> PickAudioFiles() {
  std::vector<fs::path> out;
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::vector<wchar_t> buf(64 * 1024, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = nullptr;
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = static_cast<DWORD>(buf.size());
  ofn.lpstrFilter =
      L"Audio and video\0*.mp3;*.flac;*.ogg;*.oga;*.opus;*.m4a;*.aac;*.wav;*.wma;*.aiff;*.aif;"
      L"*.alac;*.ape;*.wv;*.webm;*.mka;*.mp4;*.m4v;*.mkv;*.mov\0All files (*.*)\0*.*\0";
  ofn.lpstrTitle = L"dekp23 Custom Music - add tracks";
  ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
              OFN_ALLOWMULTISELECT | OFN_DONTADDTORECENT;
  if (GetOpenFileNameW(&ofn)) {
    // Single file: full path. Multiple: directory\0name1\0name2\0\0
    const wchar_t* p = buf.data();
    const fs::path first(p);
    p += wcslen(p) + 1;
    if (*p == L'\0') {
      out.push_back(first);
    } else {
      while (*p) {
        out.push_back(first / p);
        p += wcslen(p) + 1;
      }
    }
  }
  if (SUCCEEDED(co)) CoUninitialize();
  return out;
}

fs::path MusicFolder() {
  PWSTR path = nullptr;
  fs::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &path)) && path) out = path;
  CoTaskMemFree(path);
  return out;
}

void OpenInExplorer(const fs::path& dir) {
  ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
#else
std::vector<fs::path> PickAudioFiles() { return {}; }
fs::path MusicFolder() {
  const char* home = std::getenv("HOME");
  return home ? fs::path(home) / "Music" : fs::path();
}
void OpenInExplorer(const fs::path&) {}
#endif

}  // namespace

// ------------------------------------------------------------------ settings

PlayerSettings PlayerSettings::Load(const fs::path& dir) {
  PlayerSettings s;
  std::ifstream in(dir / "player.ini");
  std::string line;
  while (std::getline(in, line)) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq);
    const std::string v = line.substr(eq + 1);
    try {
      if (k == "enabled") s.enabled = v == "1";
      if (k == "toasts") s.toasts = v == "1";
      if (k == "fill") s.fill_repeat = v != "original";
      if (k == "volume") s.volume_percent = std::clamp(std::stoi(v), 0, 200);
    } catch (...) {
    }
  }
  return s;
}

bool PlayerSettings::Save(const fs::path& dir) const {
  std::ofstream out(dir / "player.ini", std::ios::trunc);
  out << "# dekp23 custom music player settings\n"
      << "enabled=" << (enabled ? 1 : 0) << "\n"
      << "toasts=" << (toasts ? 1 : 0) << "\n"
      << "fill=" << (fill_repeat ? "repeat" : "original") << "\n"
      << "volume=" << volume_percent << "\n";
  return static_cast<bool>(out);
}

void PlayerSettings::ApplyTo(Engine& engine) const {
  engine.SetEnabled(enabled);
  engine.SetGain(volume_percent / 100.0f);
}

UiState& GlobalUiState() {
  static UiState state;
  return state;
}

// ------------------------------------------------------------------ toast

void NowPlayingToast::OnDraw(ImGuiIO& io) {
  bool toasts;
  {
    std::lock_guard<std::mutex> lock(GlobalUiState().mutex);
    toasts = GlobalUiState().settings.toasts;
  }
  Engine& engine = GlobalEngine();
  if (!toasts || !engine.loaded()) return;

  const auto now = Engine::Clock::now();
  const NowPlaying np = engine.GetNowPlaying(now);
  if (np.active && np.replaced && np.serial != shown_serial_) {
    shown_serial_ = np.serial;
    shown_at_ = now;
    show_until_ = now + std::chrono::seconds(6);
  }
  if (now >= show_until_ || !np.active || !np.replaced) return;

  using secs = std::chrono::duration<float>;
  const float age = secs(now - shown_at_).count();
  const float left = secs(show_until_ - now).count();
  const float alpha = std::clamp(std::min(age / 0.25f, left / 0.6f), 0.0f, 1.0f);

  ImGui::SetNextWindowPos(ImVec2(24.0f, io.DisplaySize.y - 24.0f), ImGuiCond_Always,
                          ImVec2(0.0f, 1.0f));
  ImGui::SetNextWindowBgAlpha(0.78f * alpha);
  ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, 0.0f), ImVec2(520.0f, FLT_MAX));
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 12.0f));
  ImGui::PushStyleColor(ImGuiCol_Border, kAccentDim);
  ImGui::Begin("##dekp23_now_playing", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  auto* d = imgui_drawer();
  PushFontOr(d ? d->ui_font_bold() : nullptr, 13.0f);
  ImGui::TextColored(kAccent, "NOW PLAYING");
  ImGui::SameLine();
  ImGui::TextColored(kMuted, "  %s custom music", kBrand);
  ImGui::PopFont();
  PushFontOr(d ? d->ui_font_semibold() : nullptr, 22.0f);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480.0f);
  ImGui::TextUnformatted(np.title.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  const double shown_len = std::min(np.track_seconds, np.slot_seconds);
  const float frac = shown_len > 0 ? static_cast<float>(np.position_seconds / shown_len) : 0.0f;
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
  ImGui::ProgressBar(std::clamp(frac, 0.0f, 1.0f), ImVec2(-FLT_MIN, 4.0f), "");
  ImGui::PopStyleColor();
  ImGui::TextColored(kMuted, "%s / %s   iPod slot %u", FormatTime(np.position_seconds).c_str(),
                     FormatTime(shown_len).c_str(), np.song + 1);
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}

// ------------------------------------------------------------------ menu

CustomMusicMenu::CustomMusicMenu(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
  SetDrawActive(false);
}

CustomMusicMenu::~CustomMusicMenu() {
  cancel_ = true;
  if (worker_.joinable()) worker_.join();
}

void CustomMusicMenu::Show() {
  visible_ = true;
  SetDrawActive(true);
}

void CustomMusicMenu::Hide() {
  if (!visible_) return;
  visible_ = false;
  SetDrawActive(false);
  if (on_hidden_) on_hidden_();
}

void CustomMusicMenu::SetStatus(const std::string& s, bool error) {
  std::lock_guard<std::mutex> lock(job_mutex_);
  job_status_ = s;
  job_error_ = error;
}

void CustomMusicMenu::StartJob(Job job) {
  if (job_running_.exchange(true)) return;
  if (worker_.joinable()) worker_.join();
  cancel_ = false;
  {
    std::lock_guard<std::mutex> lock(job_mutex_);
    job_progress_ = -1.0f;
  }
  worker_ = std::thread([this, job] {
    RunJob(job);
    job_running_ = false;
  });
}

void CustomMusicMenu::RunJob(Job job) {
  fs::path dir, game_music;
  PlayerSettings settings;
  {
    std::lock_guard<std::mutex> lock(GlobalUiState().mutex);
    dir = GlobalUiState().library_dir;
    game_music = GlobalUiState().game_music_dir;
    settings = GlobalUiState().settings;
  }
  const fs::path tracks = dir / library::kTracksDirName;

  if (job == Job::kAddFiles) {
    SetStatus("Choose tracks in the file picker...");
    const auto files = PickAudioFiles();
    if (files.empty()) {
      SetStatus("No files chosen.");
      return;
    }
    std::vector<std::string> errors;
    const size_t n = library::ImportFiles(files, tracks, &errors);
    SetStatus("Added " + std::to_string(n) + " file(s). Converting...");
  } else if (job == Job::kImportMusicFolder) {
    const fs::path music = MusicFolder();
    const auto files = library::ScanFolder(music);
    std::vector<std::string> errors;
    const size_t n = library::ImportFiles(files, tracks, &errors);
    SetStatus("Copied " + std::to_string(n) + " new file(s) from " + PathToUtf8(music) +
              ". Converting...");
  }

  library::Options opt;
  opt.ffmpeg = library::FindFfmpeg({});
  opt.fill_repeat = settings.fill_repeat;
  opt.game_music_dir = game_music;
  const auto result = library::Rebuild(
      dir, opt,
      [this](size_t done, size_t total, const std::string& current) {
        std::lock_guard<std::mutex> lock(job_mutex_);
        job_progress_ = total ? static_cast<float>(done) / static_cast<float>(total) : -1.0f;
        job_status_ = current.empty() ? "Assigning slots..."
                                      : "Converting " + std::to_string(done + 1) + "/" +
                                            std::to_string(total) + ": " + current;
        job_error_ = false;
      },
      &cancel_);
  if (!result.ok) {
    SetStatus("Rebuild failed: " + result.message, true);
    REXLOG_WARN("custom music: rebuild failed: {}", result.message);
    return;
  }
  const bool loaded = GlobalEngine().Load(dir, [](LogLevel, const std::string& m) {
    REXLOG_INFO("{}", m);
  });
  const size_t pruned = loaded ? library::RemoveUnusedPcm(dir) : 0;
  std::string msg = "Done: " + result.message + " (" + std::to_string(result.converted) +
                    " converted, " + std::to_string(result.reused) + " cached";
  if (result.failed) msg += ", " + std::to_string(result.failed) + " failed";
  if (pruned) msg += ", " + std::to_string(pruned) + " old file(s) removed";
  msg += ")";
  if (result.layout_written) {
    // Song lengths are read by the game at startup; compare with what is running.
    const auto running = GlobalEngine().chains();
    bool same = running.size() == result.chains.size();
    for (size_t i = 0; same && i < running.size(); ++i) {
      same = result.chains[i].empty() || running[i] == result.chains[i];
    }
    if (!same) {
      msg += "\nNew song lengths take effect after restarting Skate 3.";
      std::lock_guard<std::mutex> lock(job_mutex_);
      restart_needed_ = true;
    }
  }
  if (!result.errors.empty()) msg += "\n" + result.errors.front();
  SetStatus(msg, result.failed > 0 || !loaded);
  {
    std::lock_guard<std::mutex> lock(job_mutex_);
    job_progress_ = -1.0f;
  }
  REXLOG_INFO("custom music: {}", msg);
}

void CustomMusicMenu::OnDraw(ImGuiIO& io) {
  if (!visible_) return;
  Engine& engine = GlobalEngine();
  auto* d = imgui_drawer();
  fs::path dir;
  PlayerSettings settings;
  {
    std::lock_guard<std::mutex> lock(GlobalUiState().mutex);
    dir = GlobalUiState().library_dir;
    settings = GlobalUiState().settings;
  }
  const PlayerSettings before = settings;

  ImGui::SetNextWindowSize(ImVec2(760.0f, 640.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.12f, 0.08f, 0.07f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, kAccent);
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kAccent);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, kAccent);
  bool open = true;
  if (!ImGui::Begin("dekp23 Custom Music  (F7)###dekp23_custom_music", &open,
                    ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    if (!open) Hide();
    return;
  }

  // ---- brand header
  PushFontOr(d ? d->ui_font_bold() : nullptr, 30.0f);
  ImGui::TextColored(kAccent, "CUSTOM MUSIC");
  ImGui::PopFont();
  ImGui::SameLine();
  PushFontOr(d ? d->ui_font_semibold() : nullptr, 16.0f);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
  ImGui::TextColored(kMuted, "by %s  |  %s", kBrand, kBrandUrl);
  ImGui::PopFont();
  ImGui::Separator();

  // ---- status
  const auto slots = engine.Slots();
  const size_t assigned = std::count_if(slots.begin(), slots.end(), [](auto& s) { return s.assigned; });
  if (!engine.loaded()) {
    ImGui::TextColored(kBad, "Not active: no music library loaded.");
    ImGui::TextWrapped("Library folder: %s\nAdd tracks or import your Music folder below to "
                       "get started.",
                       PathToUtf8(dir).c_str());
  } else {
    ImGui::TextColored(settings.enabled ? kGood : kWarn, "%s",
                       settings.enabled ? "Active" : "Paused (game soundtrack plays)");
    ImGui::SameLine();
    ImGui::TextColored(kMuted, "  %zu/%zu iPod songs replaced", assigned, slots.size());
    const NowPlaying np = engine.GetNowPlaying(Engine::Clock::now());
    if (np.active && np.replaced) {
      ImGui::TextColored(kAccent, "Now playing:");
      ImGui::SameLine();
      ImGui::TextUnformatted(np.title.c_str());
      const double len = std::min(np.track_seconds, np.slot_seconds);
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
      ImGui::ProgressBar(len > 0 ? static_cast<float>(np.position_seconds / len) : 0.0f,
                         ImVec2(-FLT_MIN, 6.0f), "");
      ImGui::PopStyleColor();
    } else if (np.active) {
      ImGui::TextColored(kMuted, "Now playing: original game song (slot %u)", np.song + 1);
    } else {
      ImGui::TextColored(kMuted, "Now playing: nothing from the iPod right now");
    }
  }
  ImGui::Spacing();

  // ---- controls
  ImGui::Checkbox("Use my music", &settings.enabled);
  ImGui::SameLine(0.0f, 24.0f);
  ImGui::Checkbox("Now-playing pop-ups", &settings.toasts);
  ImGui::SetNextItemWidth(260.0f);
  ImGui::SliderInt("Volume", &settings.volume_percent, 0, 200, "%d%%");
  int fill = settings.fill_repeat ? 0 : 1;
  ImGui::SetNextItemWidth(260.0f);
  ImGui::Combo("Leftover slots", &fill, "Repeat my tracks\0Keep game songs\0");
  settings.fill_repeat = fill == 0;
  if (settings.fill_repeat != before.fill_repeat) {
    ImGui::SameLine();
    ImGui::TextColored(kWarn, "applies on Rebuild");
  }

  ImGui::Spacing();
  const bool busy = JobRunning();
  ImGui::BeginDisabled(busy || dir.empty());
  if (ImGui::Button("Add tracks...")) StartJob(Job::kAddFiles);
  ImGui::SameLine();
  if (ImGui::Button("Import Music folder")) StartJob(Job::kImportMusicFolder);
  ImGui::SameLine();
  if (ImGui::Button("Rebuild library")) StartJob(Job::kRebuild);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Open tracks folder")) {
    std::error_code ec;
    fs::create_directories(dir / library::kTracksDirName, ec);
    OpenInExplorer(dir / library::kTracksDirName);
  }
  if (busy) {
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) cancel_ = true;
  }
  {
    std::lock_guard<std::mutex> lock(job_mutex_);
    if (busy && job_progress_ >= 0.0f) {
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
      ImGui::ProgressBar(job_progress_, ImVec2(-FLT_MIN, 0.0f));
      ImGui::PopStyleColor();
    }
    if (!job_status_.empty()) {
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(job_error_ ? kBad : kMuted, "%s", job_status_.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  ImGui::TextColored(kMuted, "Tracks folder: %s",
                     PathToUtf8(dir / library::kTracksDirName).c_str());
  bool layout_active;
  {
    std::lock_guard<std::mutex> lock(GlobalUiState().mutex);
    layout_active = GlobalUiState().layout_active;
  }
  ImGui::TextColored(layout_active ? kGood : kMuted, "Song lengths: %s",
                     layout_active ? "fitted to your tracks"
                                   : "original (Rebuild, then restart, to fit them)");
  {
    std::lock_guard<std::mutex> lock(job_mutex_);
    if (restart_needed_) {
      ImGui::TextColored(kWarn, "Restart Skate 3 to apply the new song lengths.");
    }
  }

  // ---- slot table
  ImGui::Spacing();
  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("slots", 4, tf, ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("Game song", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("Your track", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Fit", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableHeadersRow();
    const NowPlaying np = engine.GetNowPlaying(Engine::Clock::now());
    for (const auto& s : slots) {
      ImGui::TableNextRow();
      const bool playing = np.active && np.song == s.song;
      ImGui::TableNextColumn();
      ImGui::TextColored(playing ? kAccent : kMuted, playing ? "> %u" : "%u", s.song + 1);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(FormatTime(s.song_seconds).c_str());
      ImGui::TableNextColumn();
      if (s.assigned) {
        ImGui::TextUnformatted(s.title.c_str());
      } else {
        ImGui::TextColored(kMuted, "(original game song)");
      }
      ImGui::TableNextColumn();
      if (s.assigned) {
        const double diff = s.track_seconds - s.song_seconds;
        if (std::fabs(diff) < 0.5) {
          ImGui::TextColored(kGood, "exact");
        } else if (diff > 0) {
          ImGui::TextColored(kWarn, "cut %s", FormatTime(diff).c_str());
        } else {
          ImGui::TextColored(kMuted, "+%s silence", FormatTime(-diff).c_str());
        }
      }
    }
    ImGui::EndTable();
  }
  ImGui::TextColored(kMuted, "F7 closes this window  |  Skate 3 Recomp custom music by %s",
                     kBrand);
  ImGui::End();
  ImGui::PopStyleColor(4);
  ImGui::PopStyleVar(2);
  if (!open) Hide();

  if (settings.enabled != before.enabled || settings.toasts != before.toasts ||
      settings.volume_percent != before.volume_percent ||
      settings.fill_repeat != before.fill_repeat) {
    settings.ApplyTo(engine);
    std::lock_guard<std::mutex> lock(GlobalUiState().mutex);
    GlobalUiState().settings = settings;
    if (!dir.empty()) settings.Save(dir);
  }
}

}  // namespace skate3::custom_music
