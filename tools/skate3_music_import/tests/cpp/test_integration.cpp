// End-to-end: real iPod packets from Ipod_Stream.mus -> Engine (fingerprints, chains,
// slot events) -> Player (playlist) -> output frames, checked sample-exact against the
// expected playlist stream. Simulates what the trace showed in game: every segment on a
// fresh XMA context, the next segment starting before the previous one has finished
// (prefetch), and a song picked in the game's player (slot left early).
//   test_integration CUSTOM_DIR(with ipod.s3fp) IPOD_STREAM.mus WORK_DIR
#include "skate3_custom_music.h"
#include "skate3_custom_music_mus.h"
#include "skate3_custom_music_player.h"

#include <cstring>
#include <fstream>
#include <iostream>

using namespace skate3::custom_music;
static int g_failures = 0;
#define CHECK(c, m)                       \
  do {                                    \
    if (!(c)) {                           \
      std::cerr << "FAIL: " << m << "\n"; \
      ++g_failures;                       \
    }                                     \
  } while (0)

static int16_t Sample(int id, int64_t off, int ch) {
  return static_cast<int16_t>((id * 7919 + off * 31 + ch * 17) % 30000 - 15000);
}

struct Frame {
  uint32_t ctx;
  std::vector<uint8_t> packet;  // 2048 bytes
  int index;                    // frame within packet
  int32_t song;
  int64_t pos;                  // slot position (from the index's view)
};

int main(int argc, char** argv) {
  if (argc < 4) return 2;
  const std::filesystem::path custom = argv[1], mus_file = argv[2], work = argv[3];
  std::filesystem::create_directories(work);

  Engine engine;
  CHECK(engine.Load(custom, nullptr), "engine load");
  std::vector<mus::Segment> segs;
  std::string err;
  CHECK(mus::ReadTable(mus_file, &segs, &err), err);
  std::vector<uint32_t> first;
  const auto songs = mus::GroupSongs(segs, &first);

  // Playlist: originals 0..2 (fake audio) only.
  Player player;
  std::vector<int64_t> lens;
  player.SetOriginalDecoder([](uint32_t song, std::vector<int16_t>* pcm) {
    const int64_t n = 44100 * 40 + song * 1000;
    pcm->resize(size_t(n * 2));
    for (int64_t i = 0; i < n; ++i) {
      (*pcm)[size_t(2 * i)] = Sample(int(song), i, 0);
      (*pcm)[size_t(2 * i + 1)] = Sample(int(song), i, 1);
    }
    return true;
  });
  std::vector<PlaylistEntry> pl;
  for (uint32_t s = 0; s < 3; ++s) {
    PlaylistEntry e;
    e.original = s;
    e.samples = 44100 * 40 + s * 1000;
    pl.push_back(e);
    lens.push_back(e.samples);
  }
  player.SetPlaylist(pl);
  engine.SetRenderer(&player);

  // Build the frame stream: slot 8 segments 0..5, then slot 20 (picked early) 0..5.
  std::ifstream in(mus_file, std::ios::binary);
  uint32_t next_ctx = 10;
  std::vector<std::vector<Frame>> segments_frames;
  auto add_slot = [&](int song, int nseg) {
    int64_t timeline = 0;
    for (int k = 0; k < nseg; ++k) {
      std::vector<mus::Block> blocks;
      CHECK(mus::ReadBlocks(in, segs[first[size_t(song)] + size_t(k)], &blocks, &err), err);
      const int32_t lead = mus::LeadSamples(blocks);
      std::vector<Frame> fr;
      int64_t frames_before = 0;
      const uint32_t ctx = next_ctx++;
      for (const auto& b : blocks) {
        std::vector<uint8_t> pkt(2048, 0);
        std::copy(b.packet.begin(), b.packet.end(), pkt.begin());
        const int n = b.packet.empty() ? 0 : b.packet[0] >> 2;
        for (int f = 0; f < n; ++f) {
          fr.push_back({ctx, pkt, f, song, timeline + (frames_before + f) * 512 - lead});
        }
        frames_before += n;
      }
      timeline += segs[first[size_t(song)] + size_t(k)].num_samples;
      segments_frames.push_back(std::move(fr));
    }
  };
  add_slot(8, 6);
  add_slot(20, 6);

  // Interleave: segment k+1 starts when segment k has 25% of its frames left.
  std::vector<Frame> stream;
  for (size_t k = 0; k < segments_frames.size(); ++k) {
    auto& cur = segments_frames[k];
    const size_t split = cur.size() * 3 / 4;
    if (k == 0) stream.insert(stream.end(), cur.begin(), cur.begin() + long(split));
    if (k + 1 < segments_frames.size()) {
      auto& nxt = segments_frames[k + 1];
      const size_t nsplit = nxt.size() * 3 / 4;
      // Alternate the tail of k with the head of k+1.
      size_t a = split, b = 0;
      while (a < cur.size() || b < nsplit) {
        if (a < cur.size()) stream.push_back(cur[a++]);
        if (b < nsplit) stream.push_back(nxt[b++]);
      }
    } else {
      stream.insert(stream.end(), cur.begin() + long(split), cur.end());
    }
  }
  // The game picks slot 20 after 6 segments of slot 8 (6/30 of the song: left early).

  // Expected: slot 8 starts entry 0 at its first frame; slot 20 starts entry 1.
  const int64_t pos8_0 = segments_frames[0].front().pos;
  const int64_t pos20_0 = segments_frames[6].front().pos;
  size_t checked = 0, bad = 0, unmatched = 0, unloaded = 0;
  for (const auto& f : stream) {
    uint8_t out[2048];
    std::memset(out, 0xAB, sizeof out);
    player.WaitIdle();  // sources load on the worker; production renders silence until then
    const bool replaced = engine.OnFrame(f.ctx, f.packet.data(), f.index, 2, out);
    if (!replaced) {
      ++unmatched;  // short packets the index cannot identify (bridged ones are replaced)
      continue;
    }
    bool silent = true;
    for (int i = 0; i < 2048; ++i) silent &= out[i] == 0;
    if (silent) {
      ++unloaded;  // first frame of a slot: its entry was requested by this very frame
      continue;
    }
    int entry;
    int64_t off;
    if (f.song == 8) {
      entry = 0;
      off = f.pos - pos8_0;
    } else {
      entry = 1;
      off = f.pos - pos20_0;
    }
    ++checked;
    for (int i = 0; i < 1024; ++i) {
      const int64_t o = off + i / 2;
      const int16_t want = (o >= 0 && o < lens[size_t(entry)]) ? Sample(entry, o, i % 2) : 0;
      const int16_t got = static_cast<int16_t>(out[2 * i] << 8 | out[2 * i + 1]);
      if (got != want) {
        if (bad < 3) {
          std::cerr << "  mismatch song " << f.song << " pos " << f.pos << " sample " << i
                    << " got " << got << " want " << want << "\n";
        }
        ++bad;
        break;
      }
    }
  }
  std::cout << "integration: frames " << stream.size() << ", checked " << checked
            << ", unmatched " << unmatched << ", not yet loaded " << unloaded << ", bad frames " << bad
            << "\n";
  CHECK(unloaded <= 2, "only the first frame of each slot may be silent");
  CHECK(checked > 2000 && bad == 0, "every replaced frame must be the exact playlist audio");
  const auto st = player.Status();
  CHECK(st.entry == 1, "after the slot change the second entry plays");
  engine.SetRenderer(nullptr);
  if (g_failures) {
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
