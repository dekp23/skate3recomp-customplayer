// Tests the in-game MUS reader + index builder against the Python tool's index, and
// (when built with -DWITH_XMA_DECODER against the SDK) the in-process XMA decoder
// against reference WAVs decoded by vgmstream.
//   test_mus IPOD_STREAM.mus PYTHON_INDEX.s3fp OUT.s3fp [REF_WAV_DIR]
#include "skate3_custom_music.h"
#include "skate3_custom_music_mus.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

#ifdef WITH_XMA_DECODER
#include <rex/audio/xma/stream_decoder.h>
#endif

using namespace skate3::custom_music;
static int g_failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::cerr << "FAIL: " << m << "\n"; ++g_failures; } } while (0)

static std::vector<uint8_t> Slurp(const char* p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}

#ifdef WITH_XMA_DECODER
static std::vector<int16_t> ReadWav(const std::string& p) {
  auto b = Slurp(p.c_str());
  // find "data" chunk
  size_t off = 12;
  while (off + 8 <= b.size()) {
    uint32_t sz = b[off + 4] | (b[off + 5] << 8) | (b[off + 6] << 16) | (uint32_t(b[off + 7]) << 24);
    if (std::memcmp(&b[off], "data", 4) == 0) {
      std::vector<int16_t> s(sz / 2);
      std::memcpy(s.data(), &b[off + 8], sz);
      return s;
    }
    off += 8 + sz + (sz & 1);
  }
  return {};
}
#endif

int main(int argc, char** argv) {
  if (argc < 4) { std::cerr << "usage\n"; return 2; }
  std::vector<mus::Segment> segs;
  std::string err;
  CHECK(mus::ReadTable(argv[1], &segs, &err), err);
  std::cout << "segments " << segs.size() << "\n";
  const auto songs = mus::GroupSongs(segs);
  std::cout << "songs " << songs.size() << "\n";
  CHECK(segs.size() == 1380 && songs.size() == 46, "1380 segments / 46 songs expected");

  auto t0 = std::chrono::steady_clock::now();
  mus::IndexBuildStats st;
  CHECK(mus::BuildIndex(argv[1], argv[3], &st, &err), err);
  auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::cout << "index: packets " << st.packets << " unique " << st.unique << " dups " << st.duplicates
            << " short " << st.too_short << " in " << ms << " ms\n";
  CHECK(Slurp(argv[2]) == Slurp(argv[3]), "C++ index must be byte-identical to the Python index");

#ifdef WITH_XMA_DECODER
  if (argc >= 5) {
    const int extra = std::getenv("EXTRA") ? std::atoi(std::getenv("EXTRA")) : 0;
    std::cout << "alignment: lead + " << extra << "\n";
    rex::audio::XmaStreamDecoder dec;
    CHECK(dec.Open(44100, 2), "decoder open");
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<mus::Block> blocks;
    double max_err = 0, sum_err = 0;
    uint64_t compared = 0, exact = 0, len_bad = 0, total_samples = 0;
    t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < segs.size(); ++i) {
      CHECK(mus::ReadBlocks(in, segs[i], &blocks, &err), err);
      const auto padded = mus::PadPackets(blocks);
      std::vector<int16_t> pcm;
      const int frames = dec.DecodeStream(padded.data(), blocks.size(), &pcm);
      CHECK(frames > 0, "segment decode");
      const int32_t lead = mus::LeadSamples(blocks) + extra;
      const int64_t n = segs[i].num_samples;
      if (static_cast<int64_t>(pcm.size() / 2) < lead + n) { ++len_bad; continue; }
      total_samples += n;
      const auto ref = ReadWav(std::string(argv[4]) + "/" + std::to_string(i + 1) + ".wav");
      if (ref.size() != static_cast<size_t>(n * 2)) { ++len_bad; continue; }
      for (int64_t s = 0; s < n * 2; ++s) {
        const double e = std::abs(double(pcm[size_t(lead * 2 + s)]) - ref[size_t(s)]);
        max_err = std::max(max_err, e);
        sum_err += e;
        exact += e == 0;
        ++compared;
      }
    }
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "decode: " << total_samples / 44100.0 << " s of audio in " << ms / 1000 << " s; vs vgmstream: "
              << compared << " samples, exact " << 100.0 * exact / std::max<uint64_t>(1, compared)
              << "%, mean |err| " << sum_err / std::max<uint64_t>(1, compared) << ", max |err| " << max_err
              << ", length problems " << len_bad << "\n";
    CHECK(len_bad == 0, "every segment must decode to at least lead + num_samples");
  }
#endif
  if (g_failures) { std::cerr << g_failures << " failure(s)\n"; return 1; }
  std::cout << "all checks passed\n";
  return 0;
}
