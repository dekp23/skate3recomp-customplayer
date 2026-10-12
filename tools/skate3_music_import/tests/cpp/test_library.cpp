// Tests for src/skate3_custom_music_library.cpp (track import/conversion, carrier layout)
// and the engine's runtime controls.
// Build (repo root, see tests/cpp/CMakeLists.txt):
//   c++ -std=c++17 -Isrc src/skate3_custom_music.cpp src/skate3_custom_music_library.cpp
//       src/skate3_custom_music_mpf.cpp src/skate3_custom_music_mus.cpp
//       tools/skate3_music_import/tests/cpp/test_library.cpp -o test_library -pthread
// Run:  ./test_library WORK_DIR INDEX.s3fp [GAME_MUSIC_DIR]
//   WORK_DIR:       scratch folder (recreated)
//   INDEX.s3fp:     a real index (`skate3_music_import.py index ... --out`)
//   GAME_MUSIC_DIR: folder with ipod.mpf (carrier layout checks); optional
//   The ffmpeg-fallback rebuild runs only when ffmpeg is on PATH.
#include "skate3_custom_music.h"
#include "skate3_custom_music_library.h"
#include "skate3_custom_music_mpf.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
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

// Rebuild through an installed ffmpeg (the fallback converter), plus cache reuse and cleanup.
static void TestFfmpegFallback(const fs::path& work, const fs::path& index) {
  library::Options opt;
  opt.ffmpeg = library::FindFfmpeg({});
  if (opt.ffmpeg.empty()) {
    std::cout << "ffmpeg fallback: skipped (no ffmpeg on PATH)\n";
    return;
  }
  fs::remove_all(work);
  fs::create_directories(work / "tracks" / "sub");
  fs::copy_file(index, work / "ipod.s3fp");
  const std::string t = (work / "tracks").string();
  Run("ffmpeg -v error -y -f lavfi -i sine=f=440:d=7 '" + t + "/A song [hmxdNIyE4qI].ogg'");
  Run("ffmpeg -v error -y -f lavfi -i sine=f=330:d=5 '" + t + "/sub/B.mp3'");
  std::ofstream(work / "tracks" / "notes.txt") << "not audio";
  size_t calls = 0;
  auto r = library::Rebuild(work, opt, [&](size_t, size_t, const std::string&) { ++calls; }, nullptr);
  std::cout << "ffmpeg fallback: " << r.message << " converted=" << r.converted << "\n";
  CHECK(r.ok && r.tracks == 2 && r.converted == 2 && r.failed == 0, "fallback converts 2");
  CHECK(calls >= 3, "progress reported");
  std::ifstream tt(work / "tracks.tsv");
  std::string all((std::istreambuf_iterator<char>(tt)), {});
  CHECK(all.find("\tA song\t") != std::string::npos, "yt-dlp id stripped from the title");
  auto r2 = library::Rebuild(work, opt, nullptr, nullptr);
  CHECK(r2.ok && r2.reused == 2 && r2.converted == 0, "second rebuild reuses the cache");
  std::ofstream(work / "t_orphan.pcm") << "xxxx";
  CHECK(library::RemoveUnusedPcm(work) == 1, "orphan PCM removed, used ones kept");
}

// Engine controls with a renderer: gain, enable, hot reload while frames are processed.
struct ConstRenderer : Renderer {
  void OnSlotStart(const SlotEvent&) override {}
  bool Render(int32_t, int64_t, int64_t, int16_t* stereo) override {
    std::fill(stereo, stereo + 1024, int16_t{10000});
    return true;
  }
};

static void TestEngineControls(const fs::path& work, const fs::path& index) {
  fs::remove_all(work);
  fs::create_directories(work);
  fs::copy_file(index, work / "ipod.s3fp");
  Engine e;
  CHECK(e.Load(work, nullptr), "engine loads an index-only library");
  ConstRenderer r;
  e.SetRenderer(&r);
  // A real iPod packet is not needed for the controls: unknown packets pass through.
  uint8_t pkt[2048] = {};
  uint8_t pcm[2048];
  CHECK(!e.OnFrame(1, pkt, 0, 2, pcm), "unknown packet passes through");
  e.SetGain(0.5f);
  CHECK(e.gain() == 0.5f, "gain set");
  e.SetGain(5.0f);
  CHECK(e.gain() == 2.0f, "gain clamped");
  // Hot reload under load: one thread hammers OnFrame, another reloads repeatedly.
  std::atomic<bool> stop{false};
  std::thread audio([&] {
    uint8_t p[2048] = {};
    uint8_t out[2048];
    while (!stop) e.OnFrame(7, p, 0, 2, out);
  });
  for (int i = 0; i < 50; ++i) CHECK(e.Load(work, nullptr), "reload during playback");
  stop = true;
  audio.join();
  CHECK(e.Load(work / "missing", nullptr) == false && e.loaded(), "failed load keeps old state");
  e.SetRenderer(nullptr);
}

static void TestCarrier(const fs::path& work, const fs::path& index, const fs::path& game_music) {
  fs::remove_all(work);
  fs::create_directories(work);
  fs::copy_file(index, work / "ipod.s3fp");
  const auto segs = library::ReadIndexSegments(index);
  CHECK(segs.size() == 46, "segments from index");
  std::vector<uint32_t> per_song;
  for (const auto& sg : segs) per_song.push_back(static_cast<uint32_t>(sg.size()));
  std::string err;
  // Carrier layout for the playlist player.
  int64_t min_len = 0;
  const auto carrier = library::CarrierChains(segs, library::kMaxExtraNodes, &min_len);
  size_t cextra = 0;
  for (size_t s = 0; s < 46; ++s) cextra += carrier[s].size() - segs[s].size();
  std::cout << "carrier: every slot >= " << min_len / 44100.0 / 60 << " min, extra nodes " << cextra << "\n";
  CHECK(cextra <= library::kMaxExtraNodes && min_len > 44100LL * 60 * 15, "carrier slots >= 15 min within budget");
  bool changed = false;
  std::string msg;
  CHECK(library::EnsureCarrierLayout(work, game_music, &changed, &msg) && changed, "carrier written: " << msg);
  std::cout << msg << "\n";
  CHECK(library::EnsureCarrierLayout(work, game_music, &changed, &msg) && !changed, "carrier unchanged second time");
  std::ifstream cf(work / library::kLayoutFileName, std::ios::binary);
  std::vector<uint8_t> cb((std::istreambuf_iterator<char>(cf)), {});
  mpf::Graph cg;
  std::vector<mpf::SongChain> cc;
  CHECK(mpf::Parse(cb, &cg, &err) && mpf::ReadChains(cg, per_song, &cc, &err), "carrier parses: " << err);
  bool csame = cc.size() == 46;
  for (size_t s = 0; csame && s < 46; ++s) csame = cc[s].segments == carrier[s];
  CHECK(csame, "carrier chains read back");
}

// In-process converter path (as the game uses): tags/cover sidecars and tracks.tsv columns.
static void TestConverter(const fs::path& work, const fs::path& index) {
  fs::remove_all(work);
  fs::create_directories(work / "tracks");
  fs::copy_file(index, work / "ipod.s3fp");
  std::ofstream(work / "tracks" / "a.webm") << "x";
  std::ofstream(work / "tracks" / "b.mp3") << "y";
  std::ofstream(work / "tracks" / "broken.m4a") << "z";
  library::Options opt;
  int calls = 0;
  opt.converter = [&](const fs::path& src, const fs::path& dst, library::TrackTags* tags,
                      std::string* err) -> int64_t {
    ++calls;
    if (src.extension() == ".m4a") {
      *err = "unsupported";
      return -1;
    }
    std::ofstream(dst, std::ios::binary) << std::string(44100 * 4, '\0');
    if (src.stem() == "a") {
      tags->title = "Real Title";
      tags->artist = "Real Artist";
      tags->album = "LP";
      tags->cover = {0xFF, 0xD8, 0xFF};
    }
    return 44100;
  };
  auto r = library::Rebuild(work, opt, nullptr, nullptr);
  CHECK(r.ok && r.converted == 2 && r.failed == 1 && calls == 3, "converter used for every track");
  std::ifstream tt(work / "tracks.tsv");
  std::string all((std::istreambuf_iterator<char>(tt)), {});
  CHECK(all.find("Real Title\ta.webm\tReal Artist\tLP") != std::string::npos, "tags in tracks.tsv");
  CHECK(all.find("\tb\tb.mp3\t\t") != std::string::npos, "file name title when no tags");
  size_t covers = 0, tagfiles = 0;
  for (auto& e : fs::directory_iterator(work)) {
    covers += e.path().extension() == ".cover";
    tagfiles += e.path().extension() == ".tags";
  }
  CHECK(covers == 1 && tagfiles == 2, "sidecars written");
  auto r2 = library::Rebuild(work, opt, nullptr, nullptr);
  std::ifstream tt2(work / "tracks.tsv");
  std::string all2((std::istreambuf_iterator<char>(tt2)), {});
  CHECK(r2.reused == 2 && all2.find("Real Artist") != std::string::npos, "cached tracks keep their tags");
  CHECK(library::RemoveUnusedPcm(work) == 0, "sidecars of used tracks are kept");
  std::cout << "converter: ok\n";
}

int main(int argc, char** argv) {
  TestCleanTitle();
  if (argc >= 3) {
    const fs::path work = argv[1], index = argv[2];
    TestConverter(fs::path(work).concat("_conv"), index);
    TestFfmpegFallback(fs::path(work).concat("_ffmpeg"), index);
    TestEngineControls(fs::path(work).concat("_engine"), index);
    if (argc >= 4) TestCarrier(fs::path(work).concat("_carrier"), index, argv[3]);
  }
  if (g_fail) {
    std::cerr << g_fail << " check(s) failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
