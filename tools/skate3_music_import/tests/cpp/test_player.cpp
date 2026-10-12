// Tests for src/skate3_custom_music_player.cpp (playlist "radio" player).
// Build: c++ -std=c++17 -Isrc src/skate3_custom_music.cpp src/skate3_custom_music_player.cpp
//            tools/skate3_music_import/tests/cpp/test_player.cpp -pthread
// Run:   ./test_player WORK_DIR
#include "skate3_custom_music.h"
#include "skate3_custom_music_player.h"

#include <cstring>
#include <fstream>
#include <iostream>

using namespace skate3::custom_music;
static int g_failures = 0;
#define CHECK(c, m)                                   \
  do {                                                \
    if (!(c)) {                                       \
      std::cerr << "FAIL: " << m << "\n";             \
      ++g_failures;                                   \
    }                                                 \
  } while (0)

// Deterministic sample for (entry id, offset, channel).
static int16_t Sample(int id, int64_t off, int ch) {
  return static_cast<int16_t>((id * 7919 + off * 31 + ch * 17) % 30000 - 15000);
}

struct Rig {
  Player p;
  std::vector<PlaylistEntry> entries;
  std::vector<int> ids;  // entry index -> id used for samples
  explicit Rig(const std::filesystem::path& dir) {
    p.SetOriginalDecoder([](uint32_t song, std::vector<int16_t>* pcm) {
      const int64_t n = 44100 * 10 + song * 1000;  // ~10 s songs of differing length
      pcm->resize(static_cast<size_t>(n * 2));
      for (int64_t i = 0; i < n; ++i) {
        (*pcm)[size_t(2 * i)] = Sample(int(song), i, 0);
        (*pcm)[size_t(2 * i + 1)] = Sample(int(song), i, 1);
      }
      return true;
    });
    for (uint32_t s = 0; s < 3; ++s) {
      PlaylistEntry e;
      e.kind = PlaylistEntry::Kind::kOriginal;
      e.original = s;
      e.samples = 44100 * 10 + s * 1000;
      e.title = "orig" + std::to_string(s);
      entries.push_back(e);
      ids.push_back(int(s));
    }
    // Two user tracks as PCM files (id 100, 101).
    for (int t = 0; t < 2; ++t) {
      const int id = 100 + t;
      const int64_t n = 44100 * 7 + 777 * t;
      const auto path = dir / ("t" + std::to_string(t) + ".pcm");
      std::ofstream f(path, std::ios::binary);
      for (int64_t i = 0; i < n; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
          const int16_t v = Sample(id, i, ch);
          f.put(char(v & 0xFF));
          f.put(char((v >> 8) & 0xFF));
        }
      }
      PlaylistEntry e;
      e.kind = PlaylistEntry::Kind::kTrack;
      e.pcm = path;
      e.samples = n;
      e.title = "track" + std::to_string(t);
      entries.push_back(e);
      ids.push_back(id);
    }
    p.SetPlaylist(entries);
  }
  // Renders one frame, waiting for sources to load first.
  bool Frame(int32_t song, int64_t pos, int16_t* out) {
    int16_t tmp[1024];
    p.Render(song, pos, 0, tmp);  // triggers loading
    p.WaitIdle();
    return p.Render(song, pos, 0, out);
  }
};

// Expected radio stream: entries in `order`, back to back, looping.
static void Expect(const Rig& r, const std::vector<size_t>& order, int64_t radio_pos, int16_t* out) {
  int64_t total = 0;
  for (size_t e : order) total += r.entries[e].samples;
  for (int i = 0; i < 512; ++i) {
    int64_t q = (radio_pos + i) % total;
    size_t k = 0;
    while (q >= r.entries[order[k]].samples) q -= r.entries[order[k++]].samples;
    out[2 * i] = Sample(r.ids[order[k]], q, 0);
    out[2 * i + 1] = Sample(r.ids[order[k]], q, 1);
  }
}

static bool Same(const int16_t* a, const int16_t* b) { return std::memcmp(a, b, 2048) == 0; }

int main(int argc, char** argv) {
  const std::filesystem::path dir = argc > 1 ? argv[1] : "player_work";
  std::filesystem::create_directories(dir);
  const std::vector<size_t> all = {0, 1, 2, 3, 4};
  int16_t got[1024], want[1024];

  {  // 1. One long slot, two prefetching contexts: output == playlist, looping.
    Rig r(dir);
    SlotEvent e;
    e.song = 7, e.pos = -384, e.length = 44100 * 60;
    r.p.OnSlotStart(e);
    bool ok = true;
    int64_t frames = 0;
    for (int64_t pos = -384; pos < 44100 * 60; pos += 512) {
      // A second context decodes ahead and lags behind: same mapping either way.
      for (int64_t q : {pos, pos + 512 * 170}) {  // ~2 s ahead
        if (!r.Frame(7, q, got)) ok = false;
        Expect(r, all, q + 384, want);
        if (!Same(got, want)) {
          if (ok) std::cerr << "  first mismatch at slot pos " << q << " entry " << r.p.Status().entry << "\n";
          ok = false;
        }
        ++frames;
      }
      // Lagging frame from the past.
      if (pos > 512 * 50 && (pos / 512) % 7 == 0) {
        r.Frame(7, pos - 512 * 40, got);
        Expect(r, all, pos - 512 * 40 + 384, want);
        if (!Same(got, want)) ok = false;
      }
    }
    std::cout << "1 prefetch + lag: " << frames << " frames\n";
    CHECK(ok, "slot playback must equal the playlist stream (with prefetch and lag)");
  }

  {  // 2. Slot ends by itself -> the next slot continues seamlessly.
    Rig r(dir);
    SlotEvent a;
    a.song = 3, a.pos = -384, a.length = 44100 * 5;
    r.p.OnSlotStart(a);
    int64_t pos = -384;
    for (; pos + 512 < a.length; pos += 512) r.Frame(3, pos, got);
    const int64_t radio_next = pos + 384;  // radio position of the next frame
    SlotEvent b;
    b.song = 9, b.pos = -384, b.length = 44100 * 60, b.from_song = 3;
    b.from_pos = pos - 512, b.from_length = a.length;
    r.p.OnSlotStart(b);
    bool ok = true;
    for (int64_t q = -384, k = 0; k < 400; q += 512, ++k) {
      r.Frame(9, q, got);
      Expect(r, all, radio_next + k * 512, want);
      if (!Same(got, want)) ok = false;
    }
    CHECK(ok, "natural slot end must continue the playlist without a jump");
  }

  {  // 3. Song picked in the game (slot left early) -> next entry from its start.
    Rig r(dir);
    SlotEvent a;
    a.song = 1, a.pos = -384, a.length = 44100 * 600;
    r.p.OnSlotStart(a);
    int64_t pos = -384;
    for (; pos < 44100 * 1; pos += 512) r.Frame(1, pos, got);  // inside entry 0
    SlotEvent b;
    b.song = 2, b.pos = -384, b.length = 44100 * 600, b.from_song = 1;
    b.from_pos = pos - 512, b.from_length = a.length;
    r.p.OnSlotStart(b);
    r.Frame(2, -384, got);
    // Entry 1 starts at radio offset len(entry 0).
    Expect(r, all, r.entries[0].samples, want);
    CHECK(Same(got, want), "early slot change must start the next entry");
    const auto st = r.p.Status();
    CHECK(st.entry == 1, "status must show entry 1");
  }

  {  // 4. Next / Previous / disabled entries / shuffle keep the playing entry.
    Rig r(dir);
    SlotEvent a;
    a.song = 4, a.pos = -384, a.length = 44100 * 600;
    r.p.OnSlotStart(a);
    int64_t next = -384;  // slot position of the next frame to render
    auto play = [&](int64_t frames) {
      for (int64_t k = 0; k < frames; ++k, next += 512) r.Frame(4, next, got);
    };
    play(100);
    r.p.Next();
    r.Frame(4, next, got);
    next += 512;
    Expect(r, {1}, 0, want);
    CHECK(Same(got, want), "Next must start entry 1 at the next frame");
    CHECK(r.p.Status().entry == 1, "status after Next");
    play(10);
    r.p.Previous();  // < 3 s into entry 1 -> back to entry 0
    r.Frame(4, next, got);
    next += 512;
    Expect(r, {0}, 0, want);
    CHECK(Same(got, want), "Previous early in an entry must go back one");
    play(44100 * 5 / 512);  // 5 s into entry 0
    r.p.Previous();  // > 3 s -> restart entry 0
    r.Frame(4, next, got);
    next += 512;
    CHECK(Same(got, want), "Previous late in an entry must restart it");
    r.p.SetEnabled(1, false);  // the entry after 0 is now 2
    play(44100 * 11 / 512);    // past the end of entry 0 (10 s)
    CHECK(r.p.Status().entry == 2, "disabled entry must be skipped");
    r.p.SetShuffle(true);
    const int playing = r.p.Status().entry;
    play(5);
    CHECK(r.p.Status().entry == playing, "turning shuffle on keeps the playing entry");
    r.p.PlayEntry(4);
    r.Frame(4, next, got);
    next += 512;
    Expect(r, {4}, 0, want);
    CHECK(Same(got, want), "PlayEntry must start that entry");
  }

  {  // 5. Persistence: order and enabled flags survive, new tracks are appended.
    std::vector<PlaylistEntry> es;
    {
      std::ofstream t(dir / "tracks.tsv");
      t << "s3tracks\t1\nt0.pcm\t308700\tSong A\tA.mp3\nt1.pcm\t88977\tSong B\tB.mp3\n";
    }
    auto pl = LoadPlaylist(dir, {1000, 2000});
    CHECK(pl.size() == 4 && pl[2].title == "Song A", "load: originals then tracks");
    std::swap(pl[0], pl[3]);
    pl[1].enabled = false;
    CHECK(SavePlaylist(dir, pl, true), "save");
    {
      std::ofstream t(dir / "tracks.tsv", std::ios::app);
      t << "t2.pcm\t5000\tSong C\tC.mp3\n";
    }
    auto pl2 = LoadPlaylist(dir, {1000, 2000});
    CHECK(pl2.size() == 5 && pl2[0].key() == pl[0].key() && !pl2[1].enabled &&
              pl2[4].title == "Song C",
          "reload keeps order/enabled and appends new tracks");
    CHECK(LoadShuffle(dir), "shuffle flag persisted");
  }

  if (g_failures) {
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "all checks passed\n";
  return 0;
}
