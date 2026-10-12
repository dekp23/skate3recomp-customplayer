// Tests for src/skate3_custom_music_library.cpp and the engine's runtime controls.
// Build (repo root):
//   c++ -std=c++17 -O2 -Isrc src/skate3_custom_music.cpp src/skate3_custom_music_library.cpp
//       tools/skate3_music_import/tests/cpp/test_library.cpp -o test_library -pthread
// Run:  ./test_library ASSIGN_CASES.txt WORK_DIR INDEX.s3fp
//   ASSIGN_CASES: from tests/gen_assign_cases.py (Python reference results)
//   WORK_DIR:     empty scratch folder; needs ffmpeg on PATH
//   INDEX.s3fp:   a real index (from `skate3_music_import.py index`)
#include "skate3_custom_music.h"
#include "skate3_custom_music_library.h"
#include "skate3_custom_music_mpf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using namespace skate3::custom_music;

static int g_fail = 0;
#define CHECK(c, m)                                                    \
  do {                                                                 \
    if (!(c)) {                                                        \
      ++g_fail;                                                        \
      if (g_fail <= 20) std::cerr << "FAIL: " << m << " (" #c ")\n";   \
    }                                                                  \
  } while (0)

template <typename T>
static std::vector<T> ParseList(const std::string& s) {
  std::vector<T> v;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (!tok.empty()) v.push_back(static_cast<T>(std::stoll(tok)));
  }
  return v;
}

static void TestAssignMatchesPython(const std::string& cases) {
  std::ifstream in(cases);
  std::string line;
  int n = 0, bad = 0;
  while (std::getline(in, line)) {
    std::stringstream ss(line);
    std::string a, b, c, d;
    std::getline(ss, a, '|');
    std::getline(ss, b, '|');
    std::getline(ss, c, '|');
    std::getline(ss, d, '|');
    const auto got = library::Assign(ParseList<uint32_t>(a), ParseList<int64_t>(b), c == "1");
    const auto want = ParseList<int>(d);
    if (got != want) ++bad;
    ++n;
  }
  std::cout << "assign: " << n << " cases, " << bad << " mismatches vs Python\n";
  CHECK(n > 0 && bad == 0, "C++ Assign must match Python assign on every case");
}

static void TestCleanTitle() {
  CHECK(library::CleanTitle("x/500 Channels [hmxdNIyE4qI].webm") == "500 Channels", "yt-dlp id stripped");
  CHECK(library::CleanTitle("x/Song [live].mp3") == "Song [live]", "non-id brackets kept");
  CHECK(library::CleanTitle("x/Plain.flac") == "Plain", "plain stem");
}

static void Run(const std::string& cmd) {
  if (std::system(cmd.c_str()) != 0) {
    ++g_fail;
    std::cerr << "command failed: " << cmd << "\n";
  }
}

static void TestRebuildAndReload(const fs::path& work, const fs::path& index) {
  fs::remove_all(work);
  fs::create_directories(work / "tracks" / "sub");
  fs::copy_file(index, work / "ipod.s3fp");
  const std::string t = (work / "tracks").string();
  Run("ffmpeg -v error -y -f lavfi -i sine=f=440:d=7 -f lavfi -i color=c=red:s=32x32:d=7 "
      "-c:a libopus -c:v libvpx -shortest '" + t + "/A song [hmxdNIyE4qI].webm'");
  Run("ffmpeg -v error -y -f lavfi -i sine=f=330:d=5 '" + t + "/sub/B.mp3'");
  Run("ffmpeg -v error -y -f lavfi -i sine=f=220:d=3 -f lavfi -i color=c=blue:s=32x32:d=3 "
      "-c:a aac -c:v libx264 -shortest '" + t + "/C.mp4'");
  std::ofstream(work / "tracks" / "notes.txt") << "not audio";

  library::Options opt;
  opt.ffmpeg = library::FindFfmpeg({});
  CHECK(!opt.ffmpeg.empty(), "ffmpeg found on PATH");
  size_t calls = 0;
  auto r = library::Rebuild(work, opt, [&](size_t, size_t, const std::string&) { ++calls; }, nullptr);
  std::cout << "rebuild: ok=" << r.ok << " " << r.message << " converted=" << r.converted
            << " reused=" << r.reused << " failed=" << r.failed << "\n";
  CHECK(r.ok && r.tracks == 3 && r.converted == 3 && r.failed == 0, "first rebuild converts 3");
  CHECK(r.slots_assigned == 46, "repeat fills all 46 slots");
  CHECK(calls >= 4, "progress reported");

  auto r2 = library::Rebuild(work, opt, nullptr, nullptr);
  CHECK(r2.ok && r2.reused == 3 && r2.converted == 0, "second rebuild reuses cache");

  Engine e;
  CHECK(e.Load(work, nullptr), "engine loads in-game manifest");
  const auto slots = e.Slots();
  bool saw_clean = false;
  for (const auto& s : slots) saw_clean |= s.title == "A song";
  CHECK(slots.size() == 46 && saw_clean, "titles cleaned in manifest");

  // Stale PCM cleanup: add an orphan, rebuild with fill=original, then prune.
  std::ofstream(work / "t_orphan.pcm") << "xxxx";
  opt.fill_repeat = false;
  auto r3 = library::Rebuild(work, opt, nullptr, nullptr);
  CHECK(r3.ok && r3.slots_assigned == 3, "fill=original assigns one slot per track");
  CHECK(library::RemoveUnusedPcm(work) == 1, "orphan PCM removed, used ones kept");
  CHECK(e.Load(work, nullptr) && e.num_assigned() == 3, "reload picks up new manifest");

  // Hot reload under load: one thread hammers OnFrame, another reloads repeatedly.
  std::atomic<bool> stop{false};
  std::thread audio([&] {
    uint8_t pkt[2048] = {};
    uint8_t pcm[2048];
    while (!stop) e.OnFrame(7, pkt, 0, 2, pcm);
  });
  for (int i = 0; i < 50; ++i) CHECK(e.Load(work, nullptr), "reload during playback");
  stop = true;
  audio.join();
  CHECK(e.Load(work / "missing", nullptr) == false && e.loaded(), "failed load keeps old state");
}

static void TestNowPlayingAndGain(const fs::path& custom_dir, const fs::path& sim) {
  // Uses the real-data replay inputs (same as test_custom_music) to drive now-playing.
  Engine e;
  if (!e.Load(custom_dir, nullptr)) {
    std::cout << "now-playing: skipped (no custom dir)\n";
    return;
  }
  std::ifstream in(sim, std::ios::binary);
  uint32_t hdr[5];
  std::vector<uint8_t> pkt(2048);
  int16_t first_sample_unity = 0, first_sample_half = 0;
  bool got = false;
  while (in.read(reinterpret_cast<char*>(hdr), sizeof(hdr)) &&
         in.read(reinterpret_cast<char*>(pkt.data()), 2048)) {
    if (static_cast<int32_t>(hdr[1]) != 8 || hdr[0] != 1) continue;  // song 8 on context 1
    uint8_t a[2048], b[2048];
    e.SetGain(1.0f);
    e.ResetContext(1);
    if (!e.OnFrame(1, pkt.data(), 3, 2, a)) continue;
    e.SetGain(0.5f);
    e.ResetContext(1);
    e.OnFrame(1, pkt.data(), 3, 2, b);
    for (int i = 0; i < 1024 && !got; ++i) {
      const auto ua = static_cast<int16_t>(a[2 * i] << 8 | a[2 * i + 1]);
      if (std::abs(ua) > 1000) {
        first_sample_unity = ua;
        first_sample_half = static_cast<int16_t>(b[2 * i] << 8 | b[2 * i + 1]);
        got = true;
      }
    }
    if (got) break;
  }
  CHECK(got && std::abs(first_sample_half - first_sample_unity / 2) <= 1, "gain 0.5 halves samples");
  const auto np = e.GetNowPlaying(Engine::Clock::now());
  std::cout << "now-playing: active=" << np.active << " replaced=" << np.replaced << " song=" << np.song
            << " title='" << np.title << "' pos=" << np.position_seconds << "s serial=" << np.serial << "\n";
  CHECK(np.active && np.replaced && np.song == 8 && !np.title.empty(), "now-playing reports song 8");
  const auto later = e.GetNowPlaying(Engine::Clock::now() + std::chrono::seconds(5));
  CHECK(!later.active, "now-playing expires");
  e.SetEnabled(false);
  uint8_t c[2048];
  CHECK(!e.OnFrame(1, pkt.data(), 0, 2, c), "disabled engine passes through");
}

static void TestFlexible(const fs::path& index, const fs::path& game_music, const fs::path& work) {
  const auto segs = library::ReadIndexSegments(index);
  CHECK(segs.size() == 46, "segments from index");
  auto length_of = [&](size_t slot, const std::vector<uint16_t>& chain) {
    int64_t t = 0;
    for (uint16_t k : chain) t += segs[slot][k];
    return t;
  };
  auto check_fit = [&](const library::FlexibleAssignment& a, const std::vector<int64_t>& tracks,
                       const char* what) {
    size_t extra = 0, bad = 0, filled = 0;
    for (size_t s = 0; s < a.track_for_slot.size(); ++s) {
      if (a.track_for_slot[s] < 0) continue;
      ++filled;
      const auto& chain = a.chains[s];
      const int64_t len = length_of(s, chain);
      const int64_t t = tracks[static_cast<size_t>(a.track_for_slot[s])];
      const uint32_t max_seg = *std::max_element(segs[s].begin(), segs[s].end());
      if (len < t || len - t >= static_cast<int64_t>(max_seg)) ++bad;
      if (chain.empty() || chain[0] != 0) ++bad;
      if (chain.size() > segs[s].size()) extra += chain.size() - segs[s].size();
    }
    std::cout << what << ": filled=" << filled << " bad_fits=" << bad << " extra_nodes=" << extra << "\n";
    CHECK(bad == 0, what << ": every slot covers its track with < 1 segment of silence");
    CHECK(extra <= library::kMaxExtraNodes, what << ": node budget");
    return filled;
  };
  const std::vector<int64_t> few = {44100 * 7 * 60, 44100 * 5 * 60, 44100 * 200};
  CHECK(check_fit(library::AssignFlexible(segs, few, true), few, "3 tracks, repeat") == 46,
        "repeat fills all 46");
  CHECK(check_fit(library::AssignFlexible(segs, few, false), few, "3 tracks, original") == 3,
        "original keeps 43 game songs");
  const std::vector<int64_t> huge = {44100LL * 30 * 60};
  check_fit(library::AssignFlexible(segs, huge, false), huge, "30-minute track");

  // Worst case for the node budget: 46 twenty-minute tracks. Fits are capped, so only
  // check the budget and that the rewritten file stays valid.
  std::vector<int64_t> many(46, 44100LL * 20 * 60);
  const auto big = library::AssignFlexible(segs, many, true);
  size_t extra = 0;
  for (size_t s = 0; s < 46; ++s) extra += big.chains[s].size() > segs[s].size() ? big.chains[s].size() - segs[s].size() : 0;
  std::cout << "46 x 20 min: extra_nodes=" << extra << "\n";
  CHECK(extra <= library::kMaxExtraNodes, "budget respected under worst case");
  std::ifstream mf(game_music / "ipod.mpf", std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(mf)), {});
  mpf::Graph g;
  std::string err;
  CHECK(mpf::Parse(bytes, &g, &err), "parse game ipod.mpf: " << err);
  std::vector<uint32_t> per_song;
  for (const auto& sg : segs) per_song.push_back(static_cast<uint32_t>(sg.size()));
  std::vector<uint8_t> out;
  CHECK(mpf::WriteWithChains(g, per_song, big.chains, &out, &err), "worst-case rewrite: " << err);
  std::cout << "worst-case ipod.mpf: " << out.size() << " bytes\n";

  // Full rebuild with song-length fitting writes ipod_layout.mpf matching the chains.
  library::Options opt;
  opt.ffmpeg = library::FindFfmpeg({});
  opt.game_music_dir = game_music;
  const auto r = library::Rebuild(work, opt, nullptr, nullptr);
  CHECK(r.ok && r.layout_written, "rebuild writes layout: " << r.message);
  std::ifstream lf(work / library::kLayoutFileName, std::ios::binary);
  std::vector<uint8_t> lb((std::istreambuf_iterator<char>(lf)), {});
  mpf::Graph lg;
  std::vector<mpf::SongChain> lc;
  CHECK(mpf::Parse(lb, &lg, &err) && mpf::ReadChains(lg, per_song, &lc, &err), "layout parses: " << err);
  bool same = lc.size() == 46;
  for (size_t s = 0; same && s < 46; ++s) {
    const auto want = r.chains[s].empty() ? std::vector<uint16_t>() : r.chains[s];
    if (!want.empty()) same = lc[s].segments == want;
  }
  CHECK(same, "layout chains equal the assignment");
  Engine e;
  CHECK(e.Load(work, nullptr), "engine loads after flexible rebuild");
}

int main(int argc, char** argv) {
  TestCleanTitle();
  if (argc >= 2) TestAssignMatchesPython(argv[1]);
  if (argc >= 4) TestRebuildAndReload(argv[2], argv[3]);
  if (argc >= 6) TestNowPlayingAndGain(argv[4], argv[5]);
  if (argc >= 8) TestFlexible(argv[3], argv[6], argv[7]);
  if (g_fail) {
    std::cerr << g_fail << " check(s) failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
