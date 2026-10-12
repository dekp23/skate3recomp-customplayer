// Tests for src/skate3_custom_music_mpf.cpp against the real ipod.mpf.
// Build: c++ -std=c++17 -Isrc src/skate3_custom_music_mpf.cpp src/skate3_custom_music_library.cpp
//            src/skate3_custom_music.cpp tools/skate3_music_import/tests/cpp/test_mpf.cpp
// Run:   ./test_mpf IPOD.mpf INDEX.s3fp OUT_MODIFIED.mpf
#include "skate3_custom_music_library.h"
#include "skate3_custom_music_mpf.h"

#include <fstream>
#include <iostream>
#include <iterator>

using namespace skate3::custom_music;

static int g_fail = 0;
#define CHECK(c, m)                                                  \
  do {                                                               \
    if (!(c)) {                                                      \
      ++g_fail;                                                      \
      std::cerr << "FAIL: " << m << " (" #c ")\n";                   \
    }                                                                \
  } while (0)

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: test_mpf IPOD.mpf INDEX.s3fp OUT.mpf\n";
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  const auto segs = library::ReadIndexSegments(argv[2]);
  std::vector<uint32_t> per_song;
  for (const auto& s : segs) per_song.push_back(static_cast<uint32_t>(s.size()));
  CHECK(per_song.size() == 46, "46 songs in index");

  mpf::Graph g;
  std::string err;
  CHECK(mpf::Parse(bytes, &g, &err), "parse: " << err);
  std::vector<mpf::SongChain> chains;
  CHECK(mpf::ReadChains(g, per_song, &chains, &err), "chains: " << err);
  bool identity = chains.size() == 46;
  for (size_t s = 0; s < chains.size(); ++s) {
    identity = identity && chains[s].segments.size() == per_song[s];
    for (size_t k = 0; k < chains[s].segments.size(); ++k) identity = identity && chains[s].segments[k] == k;
  }
  CHECK(identity, "original chains are 0..n-1 for every song");
  std::cout << "parsed: " << g.num_nodes << " nodes, " << chains.size() << " song chains\n";

  // Identity rewrite must be byte-identical.
  std::vector<std::vector<uint16_t>> same(46);
  for (size_t s = 0; s < 46; ++s) same[s] = chains[s].segments;
  std::vector<uint8_t> out;
  CHECK(mpf::WriteWithChains(g, per_song, same, &out, &err), "identity write: " << err);
  CHECK(out == bytes, "identity rewrite reproduces the file byte for byte");

  // Song 8 longer (69 segments, repeats of 1..29), song 16 shorter (12 segments).
  std::vector<std::vector<uint16_t>> want(46);
  for (uint16_t k = 0; k < 30; ++k) want[8].push_back(k);
  for (uint16_t k = 1; k < 30; ++k) want[8].push_back(k);
  for (uint16_t k = 1; k <= 10; ++k) want[8].push_back(k);
  for (uint16_t k = 0; k < 12; ++k) want[16].push_back(k);
  CHECK(mpf::WriteWithChains(g, per_song, want, &out, &err), "modified write: " << err);
  mpf::Graph g2;
  CHECK(mpf::Parse(out, &g2, &err), "re-parse modified: " << err);
  std::vector<mpf::SongChain> chains2;
  CHECK(mpf::ReadChains(g2, per_song, &chains2, &err), "re-read chains: " << err);
  CHECK(chains2.size() == 46 && chains2[8].segments == want[8], "song 8 lengthened as requested");
  CHECK(chains2.size() == 46 && chains2[16].segments == want[16], "song 16 shortened as requested");
  bool others_same = true;
  for (size_t s = 0; s < chains2.size(); ++s) {
    if (s != 8 && s != 16) others_same = others_same && chains2[s].segments == chains[s].segments;
  }
  CHECK(others_same, "other songs unchanged");
  CHECK(g2.num_nodes == g.num_nodes + 39, "39 nodes appended (69 - 30)");
  // Everything after the nodes is the original tail, moved.
  const size_t tail_len = bytes.size() - g.event_offsets;
  CHECK(out.size() - g2.event_offsets == tail_len, "tail length preserved");
  std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char*>(out.data()),
                                                 static_cast<std::streamsize>(out.size()));
  std::cout << "modified: " << g2.num_nodes << " nodes, " << out.size() << " bytes (was "
            << bytes.size() << ")\n";
  if (g_fail) {
    std::cerr << g_fail << " check(s) failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
