#include "skate3_custom_music.h"

#include <algorithm>
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

bool Engine::Load(const std::filesystem::path& dir, const LogFn& log) {
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_.store(false, std::memory_order_release);
  index_.clear();
  song_samples_.clear();
  slots_.clear();
  contexts_.clear();
  stats_ = {};
  auto fail = [&](const std::string& msg) {
    if (log) log(LogLevel::kWarning, "custom music disabled: " + msg);
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
  song_samples_.resize(num_songs);
  for (auto& s : song_samples_) {
    if (!ReadLE(idx, &s)) return fail("truncated song table");
  }
  index_.resize(num_entries);
  for (auto& e : index_) {
    uint32_t pos_bits = 0;
    if (!ReadLE(idx, &e.hash) || !ReadLE(idx, &e.song) || !ReadLE(idx, &pos_bits)) {
      return fail("truncated index entries");
    }
    e.frame0_pos = static_cast<int32_t>(pos_bits);
    if (e.song >= num_songs) return fail("index entry references unknown song");
  }
  if (!std::is_sorted(index_.begin(), index_.end(),
                      [](const IndexEntry& a, const IndexEntry& b) { return a.hash < b.hash; })) {
    return fail("index not sorted");
  }

  // ---- manifest
  std::ifstream man(dir / "manifest.tsv");
  if (!man) return fail("cannot open manifest.tsv");
  slots_.resize(num_songs);
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
    if (!slots_[song]) ++assigned;
    slots_[song] = std::move(slot);
  }
  if (!header_ok) return fail("manifest is empty");

  loaded_.store(true, std::memory_order_release);
  if (log) {
    std::ostringstream msg;
    msg << "custom music: loaded " << index_.size() << " fingerprints, " << assigned << "/"
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
  if (!loaded() || !packet || !pcm_be || channels == 0 || channels > 2) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  ++stats_.frames_seen;

  ContextState& st = contexts_[context_id];
  const IndexEntry* hit = Lookup(Fingerprint(packet));
  int64_t pos;
  if (hit) {
    ++stats_.frames_matched;
    // Exact: decoded frame k of a packet plays at frame0_pos + k * 512. frame_index -1 is
    // a frame that began in the previous packet. Samples the game trims (decoder delay at
    // segment start, overrun at segment end) map outside [segment start, end) and are
    // discarded by the game, so the audible stream stays contiguous.
    pos = static_cast<int64_t>(hit->frame0_pos) +
          static_cast<int64_t>(frame_index) * kSamplesPerFrame;
    if (st.song != static_cast<int32_t>(hit->song) || pos != st.next_pos) {
      ++stats_.resyncs;
    }
    st.song = static_cast<int32_t>(hit->song);
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

  Slot* slot = slots_[static_cast<size_t>(st.song)].get();
  if (!slot) return false;  // song kept as original game audio

  int16_t stereo[kSamplesPerFrame * 2];
  if (!ReadPcm(*slot, pos, stereo)) return false;

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

void Engine::ResetContext(uint32_t context_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  contexts_.erase(context_id);
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
