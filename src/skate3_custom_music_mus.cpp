#include "skate3_custom_music_mus.h"

#include "skate3_custom_music.h"

#include <algorithm>
#include <cstdlib>
#include <tuple>

namespace skate3::custom_music::mus {
namespace fs = std::filesystem;
namespace {

constexpr uint64_t kTableOffset = 0x28;
constexpr uint64_t kEntrySize = 0x1C;
constexpr uint32_t kSamplesPerFrame = 512;

uint32_t BE32(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

bool ReadAt(std::istream& in, uint64_t off, void* dst, size_t n) {
  in.clear();
  in.seekg(static_cast<std::streamoff>(off));
  return static_cast<bool>(in.read(static_cast<char*>(dst), static_cast<std::streamsize>(n)));
}

void PutLE(std::string* s, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) s->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

}  // namespace

bool ReadTable(const fs::path& mus_file, std::vector<Segment>* segments, std::string* error) {
  auto fail = [&](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  std::ifstream in(mus_file, std::ios::binary);
  if (!in) return fail("cannot open " + PathToUtf8(mus_file));
  in.seekg(0, std::ios::end);
  const uint64_t size = static_cast<uint64_t>(in.tellg());
  uint8_t hdr[8];
  if (!ReadAt(in, 0, hdr, 8)) return fail("file too small");
  const uint32_t count = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (uint32_t{hdr[7]} << 24);
  if (count == 0 || count > 100000 || kTableOffset + count * kEntrySize > size) {
    return fail("MUS table size " + std::to_string(count) + " implausible");
  }
  std::vector<uint8_t> table(count * kEntrySize);
  if (!ReadAt(in, kTableOffset, table.data(), table.size())) return fail("cannot read MUS table");
  segments->clear();
  segments->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = table.data() + i * kEntrySize;
    Segment s;
    s.snr_offset = uint64_t{BE32(e + 8)} * 0x10;
    s.sns_offset = uint64_t{BE32(e + 12)} * 0x80;
    uint8_t snr[8];
    if (s.snr_offset + 8 > size || !ReadAt(in, s.snr_offset, snr, 8)) {
      return fail("segment " + std::to_string(i) + ": SNR beyond file end");
    }
    const uint32_t h1 = BE32(snr), h2 = BE32(snr + 4);
    const uint32_t version = h1 >> 28;
    s.codec = (h1 >> 24) & 0xF;
    s.channels = ((h1 >> 18) & 0x3F) + 1;
    s.sample_rate = h1 & 0x3FFFF;
    s.num_samples = h2 & 0x1FFFFFFF;
    if (version > 1 || s.sample_rate == 0 || s.sample_rate > 200000) {
      return fail("segment " + std::to_string(i) + ": bad SNR header");
    }
    segments->push_back(s);
  }
  return true;
}

bool ReadBlocks(std::istream& in, const Segment& seg, std::vector<Block>* blocks,
                std::string* error) {
  blocks->clear();
  uint64_t off = seg.sns_offset;
  for (size_t guard = 0; guard < 1000000; ++guard) {
    uint8_t h[12];
    if (!ReadAt(in, off, h, 12)) {
      if (error) *error = "SNS block header unreadable";
      return false;
    }
    const uint32_t word = BE32(h);
    const uint32_t id = word >> 24, bsize = word & 0xFFFFFF;
    const uint32_t stream_size = BE32(h + 8) / 4;
    if ((id != 0x00 && id != 0x80) || bsize < 12 || stream_size < 4 || 8 + stream_size > bsize) {
      if (error) *error = "unexpected SNS block (multi-stream EA-XMA is not supported)";
      return false;
    }
    Block b;
    b.samples = BE32(h + 4);
    b.packet.resize(stream_size - 4);
    if (!b.packet.empty() && !ReadAt(in, off + 12, b.packet.data(), b.packet.size())) {
      if (error) *error = "SNS packet unreadable";
      return false;
    }
    if (b.packet.size() > kXmaPacketBytes) {
      if (error) *error = "XMA packet larger than 2048 bytes";
      return false;
    }
    blocks->push_back(std::move(b));
    off += bsize;
    if (id == 0x80) return true;
  }
  if (error) *error = "no SNS end block";
  return false;
}

std::vector<uint8_t> PadPackets(const std::vector<Block>& blocks) {
  std::vector<uint8_t> out(blocks.size() * kXmaPacketBytes, 0);
  for (size_t i = 0; i < blocks.size(); ++i) {
    std::copy(blocks[i].packet.begin(), blocks[i].packet.end(), out.begin() + i * kXmaPacketBytes);
  }
  return out;
}

int32_t LeadSamples(const std::vector<Block>& blocks) {
  if (blocks.empty() || blocks[0].packet.empty()) return 0;
  return static_cast<int32_t>((blocks[0].packet[0] >> 2) * kSamplesPerFrame) -
         static_cast<int32_t>(blocks[0].samples);
}

std::vector<std::vector<uint32_t>> GroupSongs(const std::vector<Segment>& segments,
                                              std::vector<uint32_t>* first_segment) {
  std::vector<std::vector<uint32_t>> songs;
  if (first_segment) first_segment->clear();
  size_t i = 0;
  while (i < segments.size()) {
    const int64_t ref = segments[i].num_samples;
    size_t j = i + 1;
    while (j < segments.size() &&
           std::abs(static_cast<int64_t>(segments[j].num_samples) - ref) <= 1) {
      ++j;
    }
    std::vector<uint32_t> song;
    for (size_t k = i; k < j; ++k) song.push_back(segments[k].num_samples);
    songs.push_back(std::move(song));
    if (first_segment) first_segment->push_back(static_cast<uint32_t>(i));
    i = j;
  }
  return songs;
}

bool BuildIndex(const fs::path& mus_file, const fs::path& out_file, IndexBuildStats* stats,
                std::string* error, const std::function<bool(size_t, size_t)>& progress) {
  std::vector<Segment> segs;
  if (!ReadTable(mus_file, &segs, error)) return false;
  for (const auto& s : segs) {
    if (s.codec != kCodecEaXma) {
      if (error) *error = "not an EA-XMA bank";
      return false;
    }
  }
  std::vector<uint32_t> first;
  const auto songs = GroupSongs(segs, &first);

  struct Entry {
    uint64_t hash;
    uint16_t song, segment;
    int32_t offset;
  };
  std::vector<Entry> entries;
  std::vector<uint64_t> seen;
  IndexBuildStats st;
  std::ifstream in(mus_file, std::ios::binary);
  std::vector<Block> blocks;
  for (size_t song = 0; song < songs.size(); ++song) {
    for (size_t k = 0; k < songs[song].size(); ++k) {
      const size_t si = first[song] + k;
      std::string err;
      if (!ReadBlocks(in, segs[si], &blocks, &err)) {
        if (error) *error = "segment " + std::to_string(si) + ": " + err;
        return false;
      }
      const int32_t lead = LeadSamples(blocks);
      int64_t frames_before = 0;
      for (const auto& b : blocks) {
        const int32_t off = static_cast<int32_t>(frames_before * kSamplesPerFrame) - lead;
        frames_before += b.packet.empty() ? 0 : (b.packet[0] >> 2);
        ++st.packets;
        if (b.packet.size() < kFingerprintSkip + kFingerprintLength) {
          ++st.too_short;
          continue;
        }
        entries.push_back({Fingerprint(b.packet.data()), static_cast<uint16_t>(song),
                           static_cast<uint16_t>(k), off});
      }
      if (progress && !progress(si + 1, segs.size())) {
        if (error) *error = "cancelled";
        return false;
      }
    }
  }
  // Keep the first occurrence of a hash (in segment order), drop later duplicates.
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
  std::vector<Entry> unique;
  unique.reserve(entries.size());
  for (const auto& e : entries) {
    if (!unique.empty() && unique.back().hash == e.hash) {
      ++st.duplicates;
      continue;
    }
    unique.push_back(e);
  }
  st.unique = unique.size();

  std::string out = "S3FP";
  PutLE(&out, kIndexVersion, 4);
  PutLE(&out, kFingerprintSkip, 4);
  PutLE(&out, kFingerprintLength, 4);
  PutLE(&out, songs.size(), 4);
  PutLE(&out, unique.size(), 4);
  for (const auto& s : songs) {
    PutLE(&out, s.size(), 4);
    for (uint32_t v : s) PutLE(&out, v, 4);
  }
  for (const auto& e : unique) {
    PutLE(&out, e.hash, 8);
    PutLE(&out, e.song, 2);
    PutLE(&out, e.segment, 2);
    PutLE(&out, static_cast<uint32_t>(e.offset), 4);
  }
  const fs::path tmp = fs::path(out_file).concat(".tmp");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f.write(out.data(), static_cast<std::streamsize>(out.size()))) {
      if (error) *error = "cannot write " + PathToUtf8(tmp);
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, out_file, ec);
  if (ec) {
    if (error) *error = "cannot replace " + PathToUtf8(out_file) + ": " + ec.message();
    return false;
  }
  if (stats) *stats = st;
  return true;
}

}  // namespace skate3::custom_music::mus
