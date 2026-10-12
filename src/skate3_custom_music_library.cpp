#include "skate3_custom_music_library.h"

#include "skate3_custom_music_mus.h"

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

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> out(1);
  for (char c : line) {
    if (c == '\t') {
      out.emplace_back();
    } else if (c != '\r') {
      out.back().push_back(c);
    }
  }
  return out;
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

std::vector<std::vector<uint16_t>> CarrierChains(
    const std::vector<std::vector<uint32_t>>& segments_by_song, size_t max_extra_nodes,
    int64_t* min_slot_samples) {
  // Largest target T such that every slot can reach T with the shared node budget.
  auto nodes_for = [&](const std::vector<uint32_t>& segs, int64_t target) {
    int64_t len = 0;
    size_t n = 0;
    for (uint16_t k : ChainOfLength(segs.size(), segs.size() + max_extra_nodes)) {
      if (len >= target && n >= segs.size()) break;
      len += segs[k];
      ++n;
    }
    return n;
  };
  auto extra_for = [&](int64_t target) {
    size_t extra = 0;
    for (const auto& segs : segments_by_song) {
      if (segs.empty()) continue;
      const size_t n = nodes_for(segs, target);
      extra += n > segs.size() ? n - segs.size() : 0;
    }
    return extra;
  };
  int64_t lo = 0, hi = 44100LL * 3600 * 4;
  while (hi - lo > 44100) {
    const int64_t mid = lo + (hi - lo) / 2;
    (extra_for(mid) <= max_extra_nodes ? lo : hi) = mid;
  }
  std::vector<std::vector<uint16_t>> chains;
  int64_t min_len = -1;
  for (const auto& segs : segments_by_song) {
    if (segs.empty()) {
      chains.emplace_back();
      continue;
    }
    chains.push_back(ChainOfLength(segs.size(), nodes_for(segs, lo)));
    int64_t len = 0;
    for (uint16_t k : chains.back()) len += segs[k];
    min_len = min_len < 0 ? len : std::min(min_len, len);
  }
  if (min_slot_samples) *min_slot_samples = min_len;
  return chains;
}

bool EnsureCarrierLayout(const fs::path& library_dir, const fs::path& game_music_dir,
                         bool* changed, std::string* message) {
  if (changed) *changed = false;
  const auto segments = ReadIndexSegments(library_dir / "ipod.s3fp");
  std::ifstream mf(game_music_dir / "ipod.mpf", std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(mf)), {});
  if (segments.empty() || bytes.empty()) {
    if (message) *message = "carrier layout: index or game ipod.mpf missing";
    return false;
  }
  mpf::Graph graph;
  std::string err;
  if (!mpf::Parse(bytes, &graph, &err)) {
    if (message) *message = "carrier layout: " + err;
    return false;
  }
  std::vector<uint32_t> per_song;
  for (const auto& sg : segments) per_song.push_back(static_cast<uint32_t>(sg.size()));
  int64_t min_len = 0;
  const auto chains = CarrierChains(segments, kMaxExtraNodes, &min_len);
  std::vector<uint8_t> layout;
  if (!mpf::WriteWithChains(graph, per_song, chains, &layout, &err)) {
    if (message) *message = "carrier layout: " + err;
    return false;
  }
  const fs::path dst = library_dir / kLayoutFileName;
  {
    std::ifstream cur(dst, std::ios::binary);
    std::vector<uint8_t> existing((std::istreambuf_iterator<char>(cur)), {});
    if (existing == layout) {
      if (message) *message = "carrier layout up to date";
      return true;
    }
  }
  const fs::path tmp = library_dir / (std::string(kLayoutFileName) + ".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(layout.data()), static_cast<std::streamsize>(layout.size()));
    if (!out) {
      if (message) *message = "carrier layout: cannot write";
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, dst, ec);
  if (ec) {
    if (message) *message = "carrier layout: " + ec.message();
    return false;
  }
  if (changed) *changed = true;
  if (message) {
    *message = "carrier layout written: every iPod slot >= " +
               std::to_string(min_len / 44100 / 60) + " min (" + std::to_string(layout.size()) +
               " bytes)";
  }
  return true;
}

bool EnsureIndex(const fs::path& library_dir, const fs::path& game_music_dir,
                 std::string* message) {
  const fs::path index = library_dir / "ipod.s3fp";
  if (!ReadIndexSongSamples(index).empty()) return true;
  std::error_code ec;
  fs::create_directories(library_dir, ec);
  const fs::path mus_file = game_music_dir / "Ipod_Stream.mus";
  if (game_music_dir.empty() || !fs::is_regular_file(mus_file, ec)) {
    if (message) *message = "game file Ipod_Stream.mus not found";
    return false;
  }
  mus::IndexBuildStats st;
  std::string err;
  if (!mus::BuildIndex(mus_file, index, &st, &err)) {
    if (message) *message = "cannot build ipod.s3fp: " + err;
    return false;
  }
  if (message) {
    *message = "built ipod.s3fp from the game's music (" + std::to_string(st.unique) +
               " fingerprints)";
  }
  return true;
}

BuildResult Rebuild(const fs::path& library_dir, const Options& options,
                    const ProgressFn& progress, const std::atomic<bool>* cancel) {
  BuildResult r;
  std::string index_msg;
  if (!EnsureIndex(library_dir, options.game_music_dir, &index_msg)) {
    r.message = index_msg;
    return r;
  }
  const auto songs = ReadIndexSongSamples(library_dir / "ipod.s3fp");
  if (songs.empty()) {
    r.message = "ipod.s3fp invalid";
    return r;
  }
  if (!options.converter && options.ffmpeg.empty()) {
    r.message = "no audio converter available";
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
    std::string source, artist, album;
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
    TrackTags tags;
    const fs::path tags_file = fs::path(pcm).concat(".tags");
    if (!ec && size >= 4 && size % 4 == 0) {
      samples = static_cast<int64_t>(size / 4);
      ++r.reused;
      std::ifstream tf(tags_file);
      std::string line;
      if (std::getline(tf, line)) {
        const auto f = SplitTabs(line);
        if (f.size() >= 3) tags.title = f[0], tags.artist = f[1], tags.album = f[2];
      }
    } else {
      std::string err;
      if (options.converter) samples = options.converter(src, pcm, &tags, &err);
      if (samples < 0 && !options.ffmpeg.empty()) {
        // Fallback for formats the built-in decoders do not cover: an installed ffmpeg.
        std::string err2;
        samples = ConvertToPcm(options.ffmpeg, src, pcm, &err2);
        if (samples < 0 && err.empty()) err = err2;
      }
      if (samples < 0) {
        ++r.failed;
        r.errors.push_back(title + ": " + err);
        continue;
      }
      ++r.converted;
      // Sidecars: tags (title<TAB>artist<TAB>album) and the embedded cover, if any.
      auto clean = [](std::string v) {
        for (char& c : v) {
          if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        }
        return v;
      };
      std::ofstream(tags_file, std::ios::trunc)
          << clean(tags.title) << '\t' << clean(tags.artist) << '\t' << clean(tags.album) << '\n';
      if (!tags.cover.empty()) {
        std::ofstream cf(fs::path(pcm).concat(".cover"), std::ios::binary | std::ios::trunc);
        cf.write(reinterpret_cast<const char*>(tags.cover.data()),
                 static_cast<std::streamsize>(tags.cover.size()));
      }
    }
    built.push_back({name, tags.title.empty() ? title : tags.title, samples,
                     PathToUtf8(src.filename()), tags.artist, tags.album});
  }
  if (progress) progress(sources.size(), sources.size(), "");
  if (built.empty()) {
    r.message = "no track could be converted";
    return r;
  }

  // Track list for the playlist player.
  {
    const fs::path ttmp = library_dir / "tracks.tsv.tmp";
    {
      std::ofstream out(ttmp, std::ios::binary | std::ios::trunc);
      out << "# dekp23 custom music tracks: pcm<TAB>samples<TAB>title<TAB>source<TAB>artist<TAB>album\n"
          << "s3tracks\t1\n";
      for (size_t i = 0; i < built.size(); ++i) {
        out << built[i].pcm << '\t' << built[i].samples << '\t' << built[i].title << '\t'
            << built[i].source << '\t' << built[i].artist << '\t' << built[i].album << '\n';
      }
      if (!out) {
        r.message = "could not write tracks.tsv";
        return r;
      }
    }
    std::error_code tec;
    fs::rename(ttmp, library_dir / "tracks.tsv", tec);
    if (tec) {
      r.message = "could not replace tracks.tsv: " + tec.message();
      return r;
    }
  }

  r.ok = true;
  r.message = std::to_string(built.size()) + " track(s)";
  return r;
}

size_t RemoveUnusedPcm(const fs::path& library_dir) {
  std::set<std::string> used;
  {
    std::ifstream in(library_dir / "tracks.tsv");
    std::string line;
    while (std::getline(in, line)) {
      const auto a = line.find('\t');
      if (a != std::string::npos) used.insert(line.substr(0, a));
    }
  }
  size_t removed = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(library_dir, ec)) {
    const std::string ext = Lower(e.path().extension().string());
    std::string base = PathToUtf8(e.path().filename());
    if (ext == ".tags" || ext == ".cover") {
      base = PathToUtf8(e.path().stem());  // "<name>.pcm"
      if (Lower(fs::path(base).extension().string()) != ".pcm") continue;
    } else if (ext != ".pcm") {
      continue;
    }
    if (used.count(base)) continue;
    if (fs::remove(e.path(), ec)) ++removed;
  }
  return removed;
}

}  // namespace skate3::custom_music::library
