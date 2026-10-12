#include "skate3_custom_music_library.h"

#include "skate3_custom_music.h"
#include "skate3_custom_music_mpf.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace skate3::custom_music::library {
namespace fs = std::filesystem;

namespace {

std::string Lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

uint64_t Fnv1a(const std::string& s) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001B3ull;
  }
  return h;
}

std::string CacheName(const fs::path& src) {
  std::error_code ec;
  const auto size = fs::file_size(src, ec);
  const auto mtime = fs::last_write_time(src, ec).time_since_epoch().count();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "t_%016llx.pcm",
                static_cast<unsigned long long>(
                    Fnv1a(PathToUtf8(src) + "|" + std::to_string(size) + "|" + std::to_string(mtime))));
  return buf;
}

std::string Sanitize(std::string s) {
  for (auto& c : s) {
    if (c == '\t' || c == '\r' || c == '\n') c = ' ';
  }
  return s;
}

#if defined(_WIN32)
std::wstring QuoteArg(const std::wstring& a) {
  // Windows paths cannot contain '"', so plain quoting is sufficient here.
  return L"\"" + a + L"\"";
}

int RunProcess(const fs::path& exe, const std::vector<std::wstring>& args, std::string* error) {
  std::wstring cmd = QuoteArg(exe.wstring());
  for (const auto& a : args) cmd += L" " + QuoteArg(a);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (!CreateProcessW(exe.wstring().c_str(), buf.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
    if (error) *error = "could not start ffmpeg (error " + std::to_string(GetLastError()) + ")";
    return -1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return static_cast<int>(code);
}
#else
int RunProcess(const fs::path& exe, const std::vector<std::string>& args, std::string* error) {
  std::vector<std::string> all{exe.string()};
  all.insert(all.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& a : all) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
    if (error) *error = "could not start ffmpeg";
    return -1;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

}  // namespace

bool IsAudioFile(const fs::path& p) {
  static const std::set<std::string> kExt = {
      ".mp3", ".flac", ".ogg", ".oga",  ".opus", ".m4a", ".aac", ".wav", ".wma", ".aiff",
      ".aif", ".alac", ".ape", ".wv",   ".webm", ".mka", ".mp4", ".m4v", ".mkv", ".mov"};
  return kExt.count(Lower(p.extension().string())) != 0;
}

std::vector<fs::path> ScanFolder(const fs::path& folder) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return out;
  for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec) && IsAudioFile(it->path())) out.push_back(it->path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::string CleanTitle(const fs::path& file) {
  std::string t = PathToUtf8(file.stem());
  // yt-dlp default output ends in " [<11-char id>]".
  if (t.size() >= 14 && t.back() == ']') {
    const size_t open = t.size() - 13;
    if (t[open] == '[' && t[open - 1] == ' ') {
      bool id = true;
      for (size_t i = open + 1; i < t.size() - 1; ++i) {
        const char c = t[i];
        id = id && (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_');
      }
      if (id) t.resize(open - 1);
    }
  }
  return Sanitize(t);
}

fs::path FindFfmpeg(const fs::path& configured) {
  std::error_code ec;
  if (!configured.empty() && fs::is_regular_file(configured, ec)) return configured;
#if defined(_WIN32)
  wchar_t buf[MAX_PATH];
  if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr) > 0) return fs::path(buf);
  // winget installs: command-line alias links, or the package folder itself (the game
  // may have been started before winget updated PATH).
  wchar_t local[MAX_PATH];
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) > 0) {
    const fs::path base = fs::path(local) / "Microsoft" / "WinGet";
    const fs::path link = base / "Links" / "ffmpeg.exe";
    if (fs::exists(link, ec)) return link;
    const fs::path packages = base / "Packages";
    if (fs::is_directory(packages, ec)) {
      for (const auto& pkg : fs::directory_iterator(packages, ec)) {
        if (Lower(PathToUtf8(pkg.path().filename())).rfind("gyan.ffmpeg", 0) != 0) continue;
        for (fs::recursive_directory_iterator it(pkg.path(), ec), end; it != end; it.increment(ec)) {
          if (ec) break;
          if (Lower(PathToUtf8(it->path().filename())) == "ffmpeg.exe") return it->path();
        }
      }
    }
  }
#else
  if (const char* path = std::getenv("PATH")) {
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
      const fs::path candidate = fs::path(dir) / "ffmpeg";
      if (fs::is_regular_file(candidate, ec)) return candidate;
    }
  }
#endif
  return {};
}

int64_t ConvertToPcm(const fs::path& ffmpeg, const fs::path& src, const fs::path& dst,
                     std::string* error) {
  const fs::path tmp = fs::path(dst).concat(".tmp");
  std::error_code ec;
  fs::remove(tmp, ec);
#if defined(_WIN32)
  const std::vector<std::wstring> args = {
      L"-nostdin", L"-v", L"error", L"-y", L"-i", src.wstring(), L"-vn", L"-map", L"0:a:0",
      L"-ac", L"2", L"-ar", L"44100", L"-f", L"s16le", L"-acodec", L"pcm_s16le", tmp.wstring()};
#else
  const std::vector<std::string> args = {
      "-nostdin", "-v", "error", "-y", "-i", src.string(), "-vn", "-map", "0:a:0",
      "-ac", "2", "-ar", "44100", "-f", "s16le", "-acodec", "pcm_s16le", tmp.string()};
#endif
  const int code = RunProcess(ffmpeg, args, error);
  if (code != 0) {
    if (error && error->empty()) *error = "ffmpeg failed (exit " + std::to_string(code) + ")";
    fs::remove(tmp, ec);
    return -1;
  }
  const auto size = fs::file_size(tmp, ec);
  if (ec || size < 4) {
    if (error) *error = "no audio decoded";
    fs::remove(tmp, ec);
    return -1;
  }
  fs::rename(tmp, dst, ec);
  if (ec) {
    if (error) *error = "could not write " + PathToUtf8(dst.filename());
    return -1;
  }
  return static_cast<int64_t>(size / 4);
}

std::vector<int> Assign(const std::vector<uint32_t>& song_samples,
                        const std::vector<int64_t>& track_samples, bool fill_repeat) {
  std::vector<int> out(song_samples.size(), -1);
  std::map<size_t, int64_t> free;  // slot -> length
  for (size_t i = 0; i < song_samples.size(); ++i) free[i] = song_samples[i];

  std::vector<int> order(track_samples.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
  std::stable_sort(order.begin(), order.end(),
                   [&](int a, int b) { return track_samples[a] > track_samples[b]; });

  // Python: min((n, i)) over fitting slots, else max((n, i)) over all free slots.
  for (int t : order) {
    if (free.empty()) break;
    bool have = false;
    std::pair<int64_t, size_t> best{};
    for (const auto& [slot, len] : free) {
      if (len >= track_samples[t]) {
        const std::pair<int64_t, size_t> key{len, slot};
        if (!have || key < best) best = key, have = true;
      }
    }
    if (!have) {
      for (const auto& [slot, len] : free) {
        const std::pair<int64_t, size_t> key{len, slot};
        if (!have || key > best) best = key, have = true;
      }
    }
    out[best.second] = t;
    free.erase(best.second);
  }
  if (fill_repeat && !order.empty()) {
    while (!free.empty()) {
      std::pair<int64_t, size_t> slot_key{-1, 0};
      for (const auto& [slot, len] : free) slot_key = std::max(slot_key, std::make_pair(len, slot));
      // Python: max(fits, key=samples) -> first maximal in `ordered` order; else ordered[0].
      int pick = -1;
      for (int t : order) {
        if (track_samples[t] <= slot_key.first &&
            (pick < 0 || track_samples[t] > track_samples[pick])) {
          pick = t;
        }
      }
      out[slot_key.second] = pick >= 0 ? pick : order[0];
      free.erase(slot_key.second);
    }
  }
  return out;
}

std::vector<uint16_t> ChainOfLength(size_t segments, size_t n) {
  std::vector<uint16_t> chain;
  if (segments == 0 || n == 0) return chain;
  for (size_t k = 0; k < std::min(n, segments); ++k) chain.push_back(static_cast<uint16_t>(k));
  size_t next = 1;
  while (chain.size() < n) {
    if (segments == 1) {
      chain.push_back(0);
      continue;
    }
    chain.push_back(static_cast<uint16_t>(next));
    next = next + 1 < segments ? next + 1 : 1;
  }
  return chain;
}

namespace {
struct Fit {
  size_t n = 0;          // segments
  int64_t length = 0;    // slot length with n segments
};
Fit FitTrack(const std::vector<uint32_t>& segs, int64_t track, size_t max_n) {
  Fit f;
  for (uint16_t k : ChainOfLength(segs.size(), max_n)) {
    f.length += segs[k];
    ++f.n;
    if (f.length >= track) break;
  }
  return f;
}
}  // namespace

FlexibleAssignment AssignFlexible(const std::vector<std::vector<uint32_t>>& segments_by_song,
                                  const std::vector<int64_t>& track_samples, bool fill_repeat,
                                  size_t max_extra_nodes) {
  const size_t slots = segments_by_song.size();
  FlexibleAssignment a;
  a.track_for_slot.assign(slots, -1);
  a.chains.assign(slots, {});
  std::vector<bool> free(slots, true);
  size_t budget = max_extra_nodes;
  // Per-slot cap so one long track cannot use the whole node budget.
  const size_t per_slot_cap = 600;

  auto place = [&](size_t slot, int track) {
    const auto& segs = segments_by_song[slot];
    const size_t cap = std::min(per_slot_cap, segs.size() + budget);
    const Fit fit = FitTrack(segs, track_samples[static_cast<size_t>(track)], cap);
    a.track_for_slot[slot] = track;
    a.chains[slot] = ChainOfLength(segs.size(), fit.n);
    if (fit.n > segs.size()) budget -= fit.n - segs.size();
    free[slot] = false;
  };
  auto best_slot = [&](int track) -> int {
    int best = -1;
    int64_t best_pad = 0;
    for (size_t s = 0; s < slots; ++s) {
      if (!free[s]) continue;
      const auto& segs = segments_by_song[s];
      const size_t cap = std::min(per_slot_cap, segs.size() + budget);
      const Fit fit = FitTrack(segs, track_samples[static_cast<size_t>(track)], cap);
      // Truncation (cap reached) counts as heavily penalised padding.
      const int64_t t = track_samples[static_cast<size_t>(track)];
      const int64_t pad = fit.length >= t ? fit.length - t : (t - fit.length) * 4;
      if (best < 0 || pad < best_pad) {
        best = static_cast<int>(s);
        best_pad = pad;
      }
    }
    return best;
  };

  std::vector<int> order(track_samples.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
  std::stable_sort(order.begin(), order.end(),
                   [&](int x, int y) { return track_samples[x] > track_samples[y]; });
  for (int t : order) {
    const int s = best_slot(t);
    if (s < 0) break;
    place(static_cast<size_t>(s), t);
  }
  if (fill_repeat && !order.empty()) {
    for (size_t s = 0; s < slots; ++s) {
      if (!free[s]) continue;
      // Track that fits this slot with the least silence.
      int best = -1;
      int64_t best_pad = 0;
      const auto& segs = segments_by_song[s];
      for (int t : order) {
        const size_t cap = std::min(per_slot_cap, segs.size() + budget);
        const Fit fit = FitTrack(segs, track_samples[static_cast<size_t>(t)], cap);
        const int64_t len = track_samples[static_cast<size_t>(t)];
        const int64_t pad = fit.length >= len ? fit.length - len : (len - fit.length) * 4;
        if (best < 0 || pad < best_pad) best = t, best_pad = pad;
      }
      place(s, best);
    }
  }
  return a;
}

std::vector<std::vector<uint32_t>> ReadIndexSegments(const fs::path& index_file) {
  std::ifstream in(index_file, std::ios::binary);
  auto u32 = [&](uint32_t* v) {
    uint8_t b[4];
    if (!in.read(reinterpret_cast<char*>(b), 4)) return false;
    *v = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
  };
  char magic[4];
  uint32_t hdr[5];
  if (!in.read(magic, 4) || std::memcmp(magic, "S3FP", 4) != 0) return {};
  for (auto& v : hdr) {
    if (!u32(&v)) return {};
  }
  if (hdr[0] != kIndexVersion || hdr[3] == 0 || hdr[3] > 4096) return {};
  std::vector<std::vector<uint32_t>> out(hdr[3]);
  for (auto& song : out) {
    uint32_t n = 0;
    if (!u32(&n) || n == 0 || n > 4096) return {};
    song.resize(n);
    for (auto& v : song) {
      if (!u32(&v)) return {};
    }
  }
  return out;
}

std::vector<uint32_t> ReadIndexSongSamples(const fs::path& index_file) {
  std::vector<uint32_t> out;
  for (const auto& song : ReadIndexSegments(index_file)) {
    uint64_t total = 0;
    for (uint32_t v : song) total += v;
    out.push_back(static_cast<uint32_t>(total));
  }
  return out;
}

size_t ImportFiles(const std::vector<fs::path>& files, const fs::path& tracks_dir,
                   std::vector<std::string>* errors) {
  std::error_code ec;
  fs::create_directories(tracks_dir, ec);
  size_t copied = 0;
  for (const auto& f : files) {
    if (!IsAudioFile(f)) continue;
    const fs::path dst = tracks_dir / f.filename();
    if (fs::exists(dst, ec)) continue;
    fs::copy_file(f, dst, ec);
    if (ec) {
      if (errors) errors->push_back(PathToUtf8(f.filename()) + ": " + ec.message());
      continue;
    }
    ++copied;
  }
  return copied;
}

BuildResult Rebuild(const fs::path& library_dir, const Options& options,
                    const ProgressFn& progress, const std::atomic<bool>* cancel) {
  BuildResult r;
  const auto songs = ReadIndexSongSamples(library_dir / "ipod.s3fp");
  if (songs.empty()) {
    r.message = "ipod.s3fp missing or invalid (run the prepare tool once)";
    return r;
  }
  if (options.ffmpeg.empty()) {
    r.message = "ffmpeg not found (install with: winget install Gyan.FFmpeg)";
    return r;
  }
  const auto sources = ScanFolder(library_dir / kTracksDirName);
  r.tracks = sources.size();
  if (sources.empty()) {
    r.message = "no audio files in the tracks folder";
    return r;
  }

  struct Built {
    std::string pcm, title;
    int64_t samples;
  };
  std::vector<Built> built;
  for (size_t i = 0; i < sources.size(); ++i) {
    if (cancel && cancel->load()) {
      r.message = "cancelled";
      return r;
    }
    const auto& src = sources[i];
    const std::string title = CleanTitle(src);
    if (progress) progress(i, sources.size(), title);
    const std::string name = CacheName(src);
    const fs::path pcm = library_dir / name;
    std::error_code ec;
    const auto size = fs::file_size(pcm, ec);
    int64_t samples = -1;
    if (!ec && size >= 4 && size % 4 == 0) {
      samples = static_cast<int64_t>(size / 4);
      ++r.reused;
    } else {
      std::string err;
      samples = ConvertToPcm(options.ffmpeg, src, pcm, &err);
      if (samples < 0) {
        ++r.failed;
        r.errors.push_back(title + ": " + err);
        continue;
      }
      ++r.converted;
    }
    built.push_back({name, title, samples});
  }
  if (progress) progress(sources.size(), sources.size(), "");
  if (built.empty()) {
    r.message = "no track could be converted";
    return r;
  }

  std::vector<int64_t> lens;
  for (const auto& b : built) lens.push_back(b.samples);
  std::vector<int> assignment;

  // Song-length fitting needs the game's ipod.mpf to rewrite.
  std::vector<uint8_t> layout;
  if (!options.game_music_dir.empty()) {
    std::ifstream mf(options.game_music_dir / "ipod.mpf", std::ios::binary);
    std::vector<uint8_t> mpf_bytes((std::istreambuf_iterator<char>(mf)), {});
    const auto segments = ReadIndexSegments(library_dir / "ipod.s3fp");
    mpf::Graph graph;
    std::string err;
    if (!mpf_bytes.empty() && !segments.empty() && mpf::Parse(mpf_bytes, &graph, &err)) {
      std::vector<uint32_t> per_song;
      for (const auto& sg : segments) per_song.push_back(static_cast<uint32_t>(sg.size()));
      const auto flex = AssignFlexible(segments, lens, options.fill_repeat);
      if (mpf::WriteWithChains(graph, per_song, flex.chains, &layout, &err)) {
        assignment = flex.track_for_slot;
        r.chains = flex.chains;
      } else {
        r.errors.push_back("song-length layout: " + err);
        layout.clear();
      }
    } else if (!err.empty()) {
      r.errors.push_back("song-length layout: " + err);
    }
  }
  if (assignment.empty()) assignment = Assign(songs, lens, options.fill_repeat);

  const fs::path manifest = library_dir / "manifest.tsv";
  const fs::path tmp = library_dir / "manifest.tsv.tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << "# skate3recomp custom music manifest (dekp23) - written in-game\n"
        << "s3music\t1\n";
    for (size_t slot = 0; slot < assignment.size(); ++slot) {
      if (assignment[slot] < 0) continue;
      const auto& b = built[static_cast<size_t>(assignment[slot])];
      out << slot << '\t' << b.pcm << '\t' << b.samples << '\t' << b.title << '\n';
      ++r.slots_assigned;
    }
    if (!out) {
      r.message = "could not write manifest";
      return r;
    }
  }
  std::error_code ec;
  if (!layout.empty()) {
    const fs::path ltmp = library_dir / (std::string(kLayoutFileName) + ".tmp");
    {
      std::ofstream lo(ltmp, std::ios::binary | std::ios::trunc);
      lo.write(reinterpret_cast<const char*>(layout.data()),
               static_cast<std::streamsize>(layout.size()));
      if (!lo) {
        r.message = "could not write " + std::string(kLayoutFileName);
        return r;
      }
    }
    fs::rename(ltmp, library_dir / kLayoutFileName, ec);
    if (ec) {
      r.message = "could not replace " + std::string(kLayoutFileName) + ": " + ec.message();
      return r;
    }
    r.layout_written = true;
  }
  fs::rename(tmp, manifest, ec);
  if (ec) {
    r.message = "could not replace manifest.tsv: " + ec.message();
    return r;
  }
  r.ok = true;
  r.message = std::to_string(built.size()) + " track(s), " + std::to_string(r.slots_assigned) +
              " slot(s) filled";
  return r;
}

size_t RemoveUnusedPcm(const fs::path& library_dir) {
  std::set<std::string> used;
  std::ifstream in(library_dir / "manifest.tsv");
  std::string line;
  while (std::getline(in, line)) {
    const auto a = line.find('\t');
    if (a == std::string::npos) continue;
    const auto b = line.find('\t', a + 1);
    used.insert(line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1));
  }
  size_t removed = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(library_dir, ec)) {
    if (Lower(e.path().extension().string()) != ".pcm") continue;
    if (used.count(PathToUtf8(e.path().filename()))) continue;
    if (fs::remove(e.path(), ec)) ++removed;
  }
  return removed;
}

}  // namespace skate3::custom_music::library
