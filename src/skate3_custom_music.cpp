#include "skate3_custom_music.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace skate3::custom_music {
namespace {

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;
// Frames whose packet is not in the index but which follow iPod frames on the same
// context are treated as part of the same song for this many frames. Covers the 117
// iPod packets too short to fingerprint and the single ambiguous duplicate.
constexpr uint32_t kMaxBridgeFrames = 4;
// PCM read-ahead per slot (stereo sample frames).
constexpr int64_t kCacheFrames = 32768;

inline uint16_t ToBigEndian16(int16_t v) {
  const auto u = static_cast<uint16_t>(v);
  return static_cast<uint16_t>((u >> 8) | (u << 8));
}

template <typename T>
bool ReadLE(std::istream& in, T* out) {
  uint8_t b[sizeof(T)];
  if (!in.read(reinterpret_cast<char*>(b), sizeof(T))) return false;
  T v = 0;
  for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(b[i]) << (8 * i);
  *out = v;
  return true;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == '\t') {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r') {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

}  // namespace

std::string PathToUtf8(const std::filesystem::path& p) {
  const auto s = p.u8string();  // std::string in C++17, std::u8string in C++20+
  return std::string(s.begin(), s.end());
}

std::filesystem::path PathFromUtf8(const std::string& s) {
#if defined(__cpp_char8_t)
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
#else
  return std::filesystem::u8path(s);
#endif
}

uint64_t Fingerprint(const uint8_t* packet) {
  uint64_t h = kFnvOffset;
  const uint8_t* p = packet + kFingerprintSkip;
  for (uint32_t i = 0; i < kFingerprintLength; ++i) {
    h ^= p[i];
    h *= kFnvPrime;
  }
  return h;
}

struct Engine::Slot {
  std::filesystem::path path;
  int64_t num_samples = 0;
  std::string title;
  std::ifstream file;
  std::vector<int16_t> cache;  // interleaved stereo
  int64_t cache_start = -1;
  int64_t cache_frames = 0;
  bool failed = false;
};

Engine::Engine() = default;
Engine::~Engine() = default;

size_t Engine::num_assigned() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<size_t>(
      std::count_if(slots_.begin(), slots_.end(), [](const auto& s) { return s != nullptr; }));
}

size_t Engine::num_songs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return songs_.size();
}

std::filesystem::path Engine::dir() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dir_;
}

std::vector<uint32_t> Engine::song_samples() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<uint32_t> out;
  for (const auto& s : songs_) out.push_back(static_cast<uint32_t>(s.length));
  return out;
}

std::vector<std::vector<uint32_t>> Engine::segment_samples() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::vector<uint32_t>> out;
  for (const auto& s : songs_) out.push_back(s.segment_samples);
  return out;
}

std::vector<std::vector<uint16_t>> Engine::chains() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::vector<uint16_t>> out;
  for (const auto& s : songs_) out.push_back(s.chain);
  return out;
}

void Engine::ApplyChain(SongLayout& song, const std::vector<uint16_t>* chain) {
  bool valid = chain && !chain->empty();
  if (valid) {
    for (uint16_t seg : *chain) valid = valid && seg < song.segment_samples.size();
  }
  song.chain.clear();
  if (valid) {
    song.chain = *chain;
  } else {
    for (size_t k = 0; k < song.segment_samples.size(); ++k) {
      song.chain.push_back(static_cast<uint16_t>(k));
    }
  }
  song.timeline.clear();
  int64_t t = 0;
  for (uint16_t seg : song.chain) {
    song.timeline.push_back(t);
    t += song.segment_samples[seg];
  }
  song.length = t;
  song.last_started = -1;
}

void Engine::SetChains(std::vector<std::vector<uint16_t>> chains) {
  std::lock_guard<std::mutex> lock(mutex_);
  chains_override_ = std::move(chains);
  for (size_t i = 0; i < songs_.size(); ++i) {
    ApplyChain(songs_[i], i < chains_override_.size() ? &chains_override_[i] : nullptr);
  }
  contexts_.clear();
}

std::vector<SlotInfo> Engine::Slots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SlotInfo> out(songs_.size());
  for (size_t i = 0; i < out.size(); ++i) {
    out[i].song = static_cast<uint32_t>(i);
    out[i].song_seconds = songs_[i].length / kSampleRate;
    if (i < slots_.size() && slots_[i]) {
      out[i].assigned = true;
      out[i].title = slots_[i]->title;
      out[i].track_seconds = static_cast<double>(slots_[i]->num_samples) / kSampleRate;
    }
  }
  return out;
}

void Engine::SetGain(float gain) {
  gain_.store(std::clamp(gain, 0.0f, 2.0f), std::memory_order_relaxed);
}

bool Engine::Load(const std::filesystem::path& dir, const LogFn& log) {
  // Everything is parsed into locals first; the audio lock is taken only for the swap.
  auto fail = [&](const std::string& msg) {
    if (log) log(LogLevel::kWarning, "custom music: not loaded: " + msg);
    return false;
  };

  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    if (log) log(LogLevel::kInfo, "custom music: no folder at " + PathToUtf8(dir));
    return false;
  }

  // ---- index
  std::ifstream idx(dir / "ipod.s3fp", std::ios::binary);
  if (!idx) return fail("cannot open ipod.s3fp");
  char magic[4];
  uint32_t version = 0, skip = 0, length = 0, num_songs = 0, num_entries = 0;
  if (!idx.read(magic, 4) || std::memcmp(magic, "S3FP", 4) != 0) return fail("bad index magic");
  if (!ReadLE(idx, &version) || !ReadLE(idx, &skip) || !ReadLE(idx, &length) ||
      !ReadLE(idx, &num_songs) || !ReadLE(idx, &num_entries)) {
    return fail("truncated index header");
  }
  if (version != kIndexVersion || skip != kFingerprintSkip || length != kFingerprintLength) {
    return fail("index format mismatch; re-run the prepare tool");
  }
  if (num_songs == 0 || num_songs > 4096 || num_entries > 50'000'000) {
    return fail("implausible index sizes");
  }
  std::vector<SongLayout> songs(num_songs);
  for (auto& s : songs) {
    uint32_t nseg = 0;
    if (!ReadLE(idx, &nseg) || nseg == 0 || nseg > 4096) return fail("bad song table");
    s.segment_samples.resize(nseg);
    for (auto& v : s.segment_samples) {
      if (!ReadLE(idx, &v)) return fail("truncated song table");
    }
  }
  std::vector<IndexEntry> index(num_entries);
  for (auto& e : index) {
    uint32_t off_bits = 0;
    if (!ReadLE(idx, &e.hash) || !ReadLE(idx, &e.song) || !ReadLE(idx, &e.segment) ||
        !ReadLE(idx, &off_bits)) {
      return fail("truncated index entries");
    }
    e.offset = static_cast<int32_t>(off_bits);
    if (e.song >= num_songs || e.segment >= songs[e.song].segment_samples.size()) {
      return fail("index entry references unknown song/segment");
    }
  }
  if (!std::is_sorted(index.begin(), index.end(),
                      [](const IndexEntry& a, const IndexEntry& b) { return a.hash < b.hash; })) {
    return fail("index not sorted");
  }

  // ---- manifest (optional: per-slot tracks; the playlist player does not need it)
  std::ifstream man(dir / "manifest.tsv");
  std::vector<std::unique_ptr<Slot>> slots(num_songs);
  std::string line;
  bool header_ok = false;
  size_t line_no = 0, assigned = 0;
  while (std::getline(man, line)) {
    ++line_no;
    if (line.empty() || line[0] == '#') continue;
    auto cols = SplitTabs(line);
    if (!header_ok) {
      if (cols.size() < 2 || cols[0] != "s3music" || cols[1] != "1") {
        return fail("manifest header missing or unsupported");
      }
      header_ok = true;
      continue;
    }
    if (cols.size() < 3) return fail("manifest line " + std::to_string(line_no) + " malformed");
    uint32_t song = 0;
    int64_t samples = 0;
    try {
      song = static_cast<uint32_t>(std::stoul(cols[0]));
      samples = std::stoll(cols[2]);
    } catch (...) {
      return fail("manifest line " + std::to_string(line_no) + " has bad numbers");
    }
    if (song >= num_songs) return fail("manifest references song " + cols[0] + " out of range");
    if (cols[1].find("..") != std::string::npos || cols[1].find('/') != std::string::npos ||
        cols[1].find('\\') != std::string::npos) {
      return fail("manifest file names must be plain names inside the folder");
    }
    auto slot = std::make_unique<Slot>();
    slot->path = dir / PathFromUtf8(cols[1]);
    slot->num_samples = samples;
    slot->title = cols.size() > 3 ? cols[3] : cols[1];
    const auto size = std::filesystem::file_size(slot->path, ec);
    if (ec) return fail("missing PCM file " + cols[1]);
    if (static_cast<int64_t>(size / 4) < samples) {
      return fail("PCM file " + cols[1] + " shorter than manifest says");
    }
    if (!slots[song]) ++assigned;
    slots[song] = std::move(slot);
  }
  if (man && !header_ok && line_no > 0) return fail("manifest is empty");

  {
    // Old slots (and their open files) are destroyed after the swap, outside the lock.
    std::lock_guard<std::mutex> lock(mutex_);
    dir_ = dir;
    index_.swap(index);
    for (size_t i = 0; i < songs.size(); ++i) {
      ApplyChain(songs[i], i < chains_override_.size() ? &chains_override_[i] : nullptr);
    }
    songs_.swap(songs);
    slots_.swap(slots);
    contexts_.clear();
    last_song_ = -1;
    loaded_.store(true, std::memory_order_release);
  }
  if (log) {
    std::ostringstream msg;
    msg << "custom music: loaded " << num_entries << " fingerprints, " << assigned << "/"
        << num_songs << " iPod songs replaced, from " << PathToUtf8(dir);
    log(LogLevel::kInfo, msg.str());
  }
  return true;
}

const Engine::IndexEntry* Engine::Lookup(uint64_t hash) const {
  auto it = std::lower_bound(index_.begin(), index_.end(), hash,
                             [](const IndexEntry& e, uint64_t h) { return e.hash < h; });
  return (it != index_.end() && it->hash == hash) ? &*it : nullptr;
}

bool Engine::ReadPcm(Slot& slot, int64_t pos, int16_t* out) {
  // out: kSamplesPerFrame stereo frames. Beyond end of track -> silence.
  std::fill(out, out + kSamplesPerFrame * 2, int16_t{0});
  if (slot.failed || pos >= slot.num_samples || pos + kSamplesPerFrame <= 0) return true;
  if (!slot.file.is_open()) {
    slot.file.open(slot.path, std::ios::binary);
    if (!slot.file) {
      slot.failed = true;
      return false;
    }
  }
  for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
    const int64_t p = pos + i;
    if (p < 0) continue;
    if (p >= slot.num_samples) break;
    if (slot.cache_start < 0 || p < slot.cache_start || p >= slot.cache_start + slot.cache_frames) {
      const int64_t start = p;
      const int64_t count = std::min<int64_t>(kCacheFrames, slot.num_samples - start);
      slot.cache.resize(static_cast<size_t>(count * 2));
      slot.file.clear();
      slot.file.seekg(start * 4);
      std::vector<uint8_t> raw(static_cast<size_t>(count * 4));
      if (!slot.file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
        slot.failed = true;
        return false;
      }
      for (size_t k = 0; k < slot.cache.size(); ++k) {
        slot.cache[k] = static_cast<int16_t>(raw[2 * k] | (raw[2 * k + 1] << 8));  // s16le
      }
      slot.cache_start = start;
      slot.cache_frames = count;
    }
    const size_t c = static_cast<size_t>((p - slot.cache_start) * 2);
    out[2 * i] = slot.cache[c];
    out[2 * i + 1] = slot.cache[c + 1];
  }
  return true;
}

bool Engine::OnFrame(uint32_t context_id, const uint8_t* packet, int frame_index,
                     uint32_t channels, uint8_t* pcm_be) {
  if (!loaded() || !packet || !pcm_be || channels == 0 || channels > 2) {
    return false;
  }
  // Position tracking runs even while replacement is switched off, so the playlist
  // position, now-playing state and trace stay correct when it is switched back on.
  const bool tracing = trace_enabled_.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.frames_seen;

  const auto now = Clock::now();
  ContextState& st = contexts_[context_id];
  const IndexEntry* hit = Lookup(Fingerprint(packet));
  int64_t pos;
  if (hit) {
    ++stats_.frames_matched;
    SongLayout& song = songs_[hit->song];
    const int32_t prev_song = st.song;
    const int32_t prev_chain_index = st.chain_index;
    const int32_t prev_last_started = song.last_started;
    const int32_t j = ResolveChainIndex(st, song, hit->segment);
    if (prev_song != static_cast<int32_t>(hit->song) || prev_chain_index != j) {
      const int64_t seg_pos = song.timeline[static_cast<size_t>(j)] + hit->offset +
                              static_cast<int64_t>(frame_index) * kSamplesPerFrame;
      const double t_ms = std::chrono::duration<double, std::milli>(now - epoch_).count();
      if (tracing) {
        TraceEvent e;
        e.kind = TraceEvent::Kind::kSegment;
        e.t_ms = t_ms;
        e.context = context_id;
        e.song = hit->song;
        e.segment = hit->segment;
        e.chain_index = j;
        e.pos = seg_pos;
        e.slot_length = song.length;
        PushTrace(e);
      }
      // A different slot, or this slot started over (first segment again).
      const bool restart = slot_song_ == static_cast<int32_t>(hit->song) && j == 0 &&
                           prev_last_started > 0;
      // A late segment of the slot that was just replaced (prefetch overlap) is not a
      // change back to it.
      const bool late_prefetch = static_cast<int32_t>(hit->song) == prev_slot_song_ &&
                                 slot_song_ != prev_slot_song_ &&
                                 now - slot_change_time_ < std::chrono::seconds(3);
      if ((slot_song_ != static_cast<int32_t>(hit->song) && !late_prefetch) || restart) {
        SlotEvent ev;
        ev.song = hit->song;
        ev.pos = seg_pos;
        ev.length = song.length;
        ev.from_song = slot_song_;
        ev.restart = restart;
        double gap_ms = 0;
        if (slot_song_ >= 0 && static_cast<size_t>(slot_song_) < songs_.size()) {
          const SongLayout& from = songs_[static_cast<size_t>(slot_song_)];
          ev.from_pos = from.last_pos;
          ev.from_length = from.length;
          gap_ms = std::chrono::duration<double, std::milli>(now - from.last_time).count();
        }
        if (tracing) {
          TraceEvent c;
          c.kind = TraceEvent::Kind::kSongChange;
          c.t_ms = t_ms;
          c.context = context_id;
          c.song = ev.song;
          c.pos = ev.pos;
          c.slot_length = ev.length;
          c.from_song = ev.from_song;
          c.from_pos = ev.from_pos;
          c.from_length = ev.from_length;
          c.gap_ms = gap_ms;
          PushTrace(c);
        }
        if (renderer_) renderer_->OnSlotStart(ev);
        if (slot_song_ != ev.song) prev_slot_song_ = slot_song_;
        slot_song_ = ev.song;
        slot_change_time_ = now;
      }
    }
    // Exact: decoded frame k of a packet plays at offset + k * 512 within its segment;
    // frame_index -1 is a frame that began in the previous packet. Samples the game trims
    // (decoder delay at segment start, overrun at segment end) map outside the segment
    // and are discarded by the game, so the audible stream stays contiguous.
    pos = song.timeline[static_cast<size_t>(j)] + static_cast<int64_t>(hit->offset) +
          static_cast<int64_t>(frame_index) * kSamplesPerFrame;
    if (st.song != static_cast<int32_t>(hit->song) || pos != st.next_pos) {
      ++stats_.resyncs;
    }
    st.song = static_cast<int32_t>(hit->song);
    st.chain_index = j;
    st.unmatched_frames = 0;
  } else if (st.song >= 0 && st.unmatched_frames < kMaxBridgeFrames) {
    ++st.unmatched_frames;
    ++stats_.frames_continued;
    pos = st.next_pos;
  } else {
    st.song = -1;
    return false;
  }
  st.next_pos = pos + kSamplesPerFrame;

  // Now-playing: a different song, or the same song after a pause, counts as a new start.
  if (st.song != last_song_ || now - last_time_ > std::chrono::seconds(3)) {
    ++serial_;
  }
  last_song_ = st.song;
  last_pos_ = pos;
  last_time_ = now;
  {
    SongLayout& cur = songs_[static_cast<size_t>(st.song)];
    cur.last_pos = std::max(cur.last_pos, pos);
    if (pos < cur.last_pos - static_cast<int64_t>(kSampleRate) * 30) cur.last_pos = pos;  // restart
    cur.last_time = now;
  }

  if (!enabled()) return false;
  int16_t stereo[kSamplesPerFrame * 2];
  if (renderer_) {
    if (!renderer_->Render(st.song, pos, songs_[static_cast<size_t>(st.song)].length, stereo)) {
      return false;
    }
  } else {
    Slot* slot = slots_[static_cast<size_t>(st.song)].get();
    if (!slot) return false;  // song kept as original game audio
    if (!ReadPcm(*slot, pos, stereo)) return false;
  }

  const float g = gain();
  if (g != 1.0f) {
    for (auto& v : stereo) {
      v = static_cast<int16_t>(std::clamp(static_cast<int>(v * g), -32768, 32767));
    }
  }

  auto* out = reinterpret_cast<uint16_t*>(pcm_be);
  if (channels == 2) {
    for (uint32_t i = 0; i < kSamplesPerFrame * 2; ++i) out[i] = ToBigEndian16(stereo[i]);
  } else {
    for (uint32_t i = 0; i < kSamplesPerFrame; ++i) {
      const int mixed = (static_cast<int>(stereo[2 * i]) + stereo[2 * i + 1]) / 2;
      out[i] = ToBigEndian16(static_cast<int16_t>(mixed));
    }
  }
  ++stats_.frames_overridden;
  return true;
}

int32_t Engine::ResolveChainIndex(ContextState& st, SongLayout& song, uint16_t segment) {
  const auto& chain = song.chain;
  const int32_t n = static_cast<int32_t>(chain.size());
  // Same segment still decoding on this context.
  if (st.song >= 0 && &songs_[static_cast<size_t>(st.song)] == &song && st.chain_index >= 0 &&
      st.chain_index < n && chain[static_cast<size_t>(st.chain_index)] == segment) {
    return st.chain_index;
  }
  // A new segment starts: the next occurrence after the most recently started entry (a
  // prefetching second context starts segments in chain order too).
  for (int32_t j = song.last_started + 1; j < n; ++j) {
    if (chain[static_cast<size_t>(j)] == segment) {
      song.last_started = j;
      return j;
    }
  }
  // Song (re)started, or played out of the expected order: first occurrence.
  for (int32_t j = 0; j < n; ++j) {
    if (chain[static_cast<size_t>(j)] == segment) {
      song.last_started = j;
      return j;
    }
  }
  // Segment not in this slot's chain (e.g. a short-version section): original position.
  song.last_started = -1;
  int32_t j = 0;
  for (; j < n && chain[static_cast<size_t>(j)] < segment; ++j) {
  }
  return std::min(j, n - 1);
}

void Engine::SetRenderer(Renderer* renderer) {
  std::lock_guard<std::mutex> lock(mutex_);
  renderer_ = renderer;
  slot_song_ = -1;
  prev_slot_song_ = -1;
}

void Engine::ResetContext(uint32_t context_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = contexts_.find(context_id);
  if (it == contexts_.end()) return;
  // Only contexts that played iPod audio are traced (resets are frequent for SFX).
  if (trace_enabled_.load(std::memory_order_relaxed) && it->second.song >= 0) {
    TraceEvent e;
    e.kind = TraceEvent::Kind::kReset;
    e.t_ms = std::chrono::duration<double, std::milli>(Clock::now() - epoch_).count();
    e.context = context_id;
    e.song = it->second.song;
    e.pos = it->second.next_pos;
    PushTrace(e);
  }
  contexts_.erase(it);
}

void Engine::PushTrace(TraceEvent e) {
  // Caller holds mutex_. Bounded so a stalled drain cannot grow memory.
  constexpr size_t kMaxTrace = 4096;
  if (trace_.size() >= kMaxTrace) {
    trace_.erase(trace_.begin(), trace_.begin() + kMaxTrace / 2);
    trace_dropped_ += kMaxTrace / 2;
  }
  trace_.push_back(e);
}

std::vector<TraceEvent> Engine::DrainTrace() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TraceEvent> out;
  out.swap(trace_);
  return out;
}

std::string FormatTrace(const TraceEvent& e) {
  char buf[256];
  const auto sec = [](int64_t samples) { return static_cast<double>(samples) / kSampleRate; };
  switch (e.kind) {
    case TraceEvent::Kind::kSegment:
      std::snprintf(buf, sizeof(buf),
                    "MUSICTRACE t=%.1f SEG ctx=%u song=%d seg=%d chain=%d pos=%.3f len=%.3f",
                    e.t_ms, e.context, e.song, e.segment, e.chain_index, sec(e.pos),
                    sec(e.slot_length));
      break;
    case TraceEvent::Kind::kSongChange:
      std::snprintf(buf, sizeof(buf),
                    "MUSICTRACE t=%.1f SONG ctx=%u from=%d from_pos=%.3f from_len=%.3f "
                    "to=%d to_pos=%.3f to_len=%.3f gap_ms=%.1f",
                    e.t_ms, e.context, e.from_song, sec(e.from_pos), sec(e.from_length), e.song,
                    sec(e.pos), sec(e.slot_length), e.gap_ms);
      break;
    case TraceEvent::Kind::kReset:
      std::snprintf(buf, sizeof(buf), "MUSICTRACE t=%.1f RESET ctx=%u song=%d pos=%.3f",
                    e.t_ms, e.context, e.song, sec(e.pos));
      break;
  }
  return buf;
}

NowPlaying Engine::GetNowPlaying(Clock::time_point now, Clock::duration window) const {
  std::lock_guard<std::mutex> lock(mutex_);
  NowPlaying np;
  np.serial = serial_;
  if (last_song_ < 0 || static_cast<size_t>(last_song_) >= songs_.size() ||
      now - last_time_ > window) {
    return np;
  }
  np.active = true;
  np.song = static_cast<uint32_t>(last_song_);
  np.position_seconds = std::max<int64_t>(0, last_pos_) / kSampleRate;
  np.slot_seconds = songs_[np.song].length / kSampleRate;
  if (const Slot* slot = slots_[np.song].get(); slot && enabled()) {
    np.replaced = true;
    np.title = slot->title;
    np.track_seconds = static_cast<double>(slot->num_samples) / kSampleRate;
  }
  return np;
}

Stats Engine::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

Engine& GlobalEngine() {
  static Engine engine;
  return engine;
}

}  // namespace skate3::custom_music
