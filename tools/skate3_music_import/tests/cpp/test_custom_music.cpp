// Standalone test for src/skate3_custom_music.cpp.
//
// Build (from repo root):
//   c++ -std=c++17 -O2 -Isrc src/skate3_custom_music.cpp
//       tools/skate3_music_import/tests/cpp/test_custom_music.cpp -o test_custom_music
// Run:
//   ./test_custom_music                       unit tests only
//   ./test_custom_music CUSTOM_DIR SIM.bin [CHAINS.txt]  + end-to-end replay of real packets
//     CUSTOM_DIR: output of `skate3_music_import.py prepare`
//     SIM.bin:    output of tests/sim_stream.py
#include "skate3_custom_music.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
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

struct Manifest {
  std::map<int, std::pair<std::string, int64_t>> slots;  // song -> (file, samples)
};

static Manifest ReadManifest(const std::string& dir) {
  Manifest m;
  std::ifstream in(dir + "/manifest.tsv");
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line.rfind("s3music", 0) == 0) continue;
    std::istringstream ss(line);
    int song;
    std::string file;
    int64_t n;
    ss >> song >> file >> n;
    m.slots[song] = {file, n};
  }
  return m;
}

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
  const Manifest man = ReadManifest(dir);

  std::map<int, std::vector<int16_t>> pcm;  // song -> stereo s16
  for (auto& [song, fs] : man.slots) {
    std::ifstream f(dir + "/" + fs.first, std::ios::binary);
    std::vector<int16_t> v(static_cast<size_t>(fs.second * 2));
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * 2));
    pcm[song] = std::move(v);
  }

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
      const bool should = song >= 0 && man.slots.count(song);
      if (should) {
        ++expected_override;
        songs_checked.insert(song);
        if (!replaced) continue;
        ++got_override;
        const auto& src = pcm.at(song);
        const int64_t pos = frame0 + static_cast<int64_t>(f) * 512;
        for (int i = 0; i < 1024; ++i) {
          const int64_t p = pos * 2 + i;
          const int16_t want = (p >= 0 && p < static_cast<int64_t>(src.size())) ? src[p] : 0;
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

  const Stats st = e.stats();
  std::cout << "replay: frames=" << frames << " expected_override=" << expected_override
            << " got_override=" << got_override << " bad_samples=" << bad_samples
            << " wrongly_overridden=" << wrong_pass << "\n"
            << "engine: seen=" << st.frames_seen << " matched=" << st.frames_matched
            << " bridged=" << st.frames_continued << " overridden=" << st.frames_overridden
            << " resyncs=" << st.resyncs << " songs_checked=" << songs_checked.size() << "\n";
  CHECK(frames > 0, "sim stream empty");
  CHECK(got_override == expected_override, "every assigned iPod frame must be overridden");
  CHECK(bad_samples == 0, "overridden PCM must equal user track at the exact position");
  CHECK(wrong_pass == 0, "non-iPod / unassigned / reset contexts must pass through");
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
