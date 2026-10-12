// Standalone test for src/skate3_custom_music.cpp.
//
// Build (from repo root):
//   c++ -std=c++17 -O2 -Isrc src/skate3_custom_music.cpp
//       tools/skate3_music_import/tests/cpp/test_custom_music.cpp -o test_custom_music
// Run:
//   ./test_custom_music                       unit tests only
//   ./test_custom_music LIBRARY_DIR SIM.bin [CHAINS.txt]  + end-to-end replay of real packets
//     LIBRARY_DIR: folder with ipod.s3fp (`skate3_music_import.py index ... --out`)
//     SIM.bin:     output of tests/sim_stream.py
//   Every replaced frame must hold the renderer's audio for exactly that slot position.
#include "skate3_custom_music.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

using namespace skate3::custom_music;

static int g_failures = 0;
#define CHECK(cond, msg)                                                   \
  do {                                                                     \
    if (!(cond)) {                                                         \
      ++g_failures;                                                        \
      if (g_failures <= 20) std::cerr << "FAIL: " << msg << " (" #cond ")\n"; \
    }                                                                      \
  } while (0)

static void TestFingerprintMatchesPython() {
  // Reference: python3 -c "from s3mus.xma import fingerprint; print(hex(fingerprint(bytes(range(256))*8)))"
  std::vector<uint8_t> pkt(2048);
  for (size_t i = 0; i < pkt.size(); ++i) pkt[i] = static_cast<uint8_t>(i & 0xFF);
  const uint64_t h = Fingerprint(pkt.data());
  std::cout << "fingerprint(range pattern) = 0x" << std::hex << h << std::dec << "\n";
  CHECK(h == 0xB4DB28C285BFD025ull, "fingerprint differs from Python reference");
}

static void TestMissingFolderIsInert() {
  Engine e;
  CHECK(!e.Load("/nonexistent/custom_music_dir", nullptr), "load of missing dir must fail");
  uint8_t pkt[2048] = {};
  uint8_t pcm[2048] = {};
  CHECK(!e.OnFrame(0, pkt, 0, 2, pcm), "inert engine must not override");
}

// Deterministic "track" audio per slot and position.
static int16_t Synth(int song, int64_t pos, int ch) {
  const int64_t v = (song * 7919 + pos * 31 + ch * 17) % 30000;
  return static_cast<int16_t>((v < 0 ? v + 30000 : v) - 15000);
}

// Renders every slot except kPassthroughSong, which keeps the game's audio.
constexpr int kPassthroughSong = 16;
struct SynthRenderer : Renderer {
  void OnSlotStart(const SlotEvent&) override { ++slot_starts; }
  bool Render(int32_t song, int64_t pos, int64_t, int16_t* stereo) override {
    if (song == kPassthroughSong) return false;
    for (int i = 0; i < 512; ++i) {
      stereo[2 * i] = Synth(song, pos + i, 0);
      stereo[2 * i + 1] = Synth(song, pos + i, 1);
    }
    return true;
  }
  int slot_starts = 0;
};

static int RunReplay(const std::string& dir, const std::string& sim_path,
                     const std::string& chains_path) {
  Engine e;
  if (!chains_path.empty()) {
    // Lines "song:seg,seg,..."
    std::vector<std::vector<uint16_t>> chains(64);
    std::ifstream cf(chains_path);
    std::string cl;
    while (std::getline(cf, cl)) {
      const auto colon = cl.find(':');
      if (colon == std::string::npos) continue;
      const int song = std::stoi(cl.substr(0, colon));
      std::stringstream ss(cl.substr(colon + 1));
      std::string tok;
      while (std::getline(ss, tok, ',')) chains[song].push_back(static_cast<uint16_t>(std::stoi(tok)));
    }
    e.SetChains(chains);
  }
  std::vector<std::string> logs;
  e.SetTraceEnabled(true);
  CHECK(e.Load(dir, [&](LogLevel, const std::string& s) { logs.push_back(s); std::cout << s << "\n"; }),
        "engine load");
  if (!e.loaded()) return 1;
  SynthRenderer renderer;
  e.SetRenderer(&renderer);

  std::ifstream sim(sim_path, std::ios::binary);
  uint64_t frames = 0, expected_override = 0, got_override = 0, bad_samples = 0, wrong_pass = 0;
  std::set<int> songs_checked;
  while (true) {
    uint32_t hdr[5];
    if (!sim.read(reinterpret_cast<char*>(hdr), sizeof(hdr))) break;
    std::vector<uint8_t> pkt(2048);
    sim.read(reinterpret_cast<char*>(pkt.data()), 2048);
    const uint32_t ctx = hdr[0];
    const int song = static_cast<int32_t>(hdr[1]);
    const int64_t frame0 = static_cast<int32_t>(hdr[2]);
    const uint32_t fcount = hdr[3];
    const uint32_t flags = hdr[4];
    if (flags & 1) e.ResetContext(ctx);
    for (uint32_t f = 0; f < fcount; ++f) {
      ++frames;
      uint8_t buf[2048];
      std::memset(buf, 0xAB, sizeof(buf));  // "original decoded audio" sentinel
      const bool replaced = e.OnFrame(ctx, pkt.data(), static_cast<int>(f), 2, buf);
      const bool should = song >= 0 && song != kPassthroughSong;
      if (should) {
        ++expected_override;
        songs_checked.insert(song);
        if (!replaced) continue;
        ++got_override;
        const int64_t pos = frame0 + static_cast<int64_t>(f) * 512;
        for (int i = 0; i < 1024; ++i) {
          const int16_t want = Synth(song, pos + i / 2, i % 2);
          const uint16_t be = static_cast<uint16_t>(buf[2 * i] << 8 | buf[2 * i + 1]);
          if (static_cast<int16_t>(be) != want) ++bad_samples;
        }
      } else if (replaced) {
        ++wrong_pass;
      }
    }
  }
  // Playback trace: segment starts and song changes must be reported.
  size_t seg_events = 0, song_events = 0, reset_events = 0;
  const auto trace = e.DrainTrace();
  for (const auto& ev : trace) {
    if (ev.kind == TraceEvent::Kind::kSegment) ++seg_events;
    if (ev.kind == TraceEvent::Kind::kSongChange) ++song_events;
    if (ev.kind == TraceEvent::Kind::kReset) ++reset_events;
  }
  for (size_t i = 0; i < trace.size() && i < 4; ++i) std::cout << FormatTrace(trace[i]) << "\n";
  std::cout << "trace: segments=" << seg_events << " song_changes=" << song_events
            << " resets=" << reset_events << "\n";
  CHECK(seg_events > 0 && song_events > 0, "trace must report segment starts and song changes");
  CHECK(renderer.slot_starts > 0, "renderer is told about slot starts");

  const Stats st = e.stats();
  std::cout << "replay: frames=" << frames << " expected_override=" << expected_override
            << " got_override=" << got_override << " bad_samples=" << bad_samples
            << " wrongly_overridden=" << wrong_pass << "\n"
            << "engine: seen=" << st.frames_seen << " matched=" << st.frames_matched
            << " bridged=" << st.frames_continued << " overridden=" << st.frames_overridden
            << " resyncs=" << st.resyncs << " songs_checked=" << songs_checked.size() << "\n";
  CHECK(frames > 0, "sim stream empty");
  CHECK(got_override == expected_override, "every rendered iPod frame must be replaced");
  CHECK(bad_samples == 0, "replaced PCM must be the renderer's audio at the exact slot position");
  CHECK(wrong_pass == 0, "non-iPod frames and passthrough slots must keep the game's audio");
  return 0;
}

int main(int argc, char** argv) {
  TestFingerprintMatchesPython();
  TestMissingFolderIsInert();
  if (argc >= 3) RunReplay(argv[1], argv[2], argc >= 4 ? argv[3] : "");
  if (g_failures) {
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
