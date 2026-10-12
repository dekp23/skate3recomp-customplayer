// Skate 3 custom music (dekp23): reader for the game's EA MUS stream bank
// (Ipod_Stream.mus) and the in-game builder of the fingerprint index.
//
// Layout (vgmstream src/meta/ea_eaac_mpf_mus.c, ea_eaac.c, ea_eaac_streamfile.h), as
// measured on Skate 3 X360 (Pathfinder v5.3, big-endian):
//   0x04 u32le  entry count;  0x28 table of 0x1C-byte entries:
//        u32 checksum, u16 index, u16 sub, u32 snr_offset/0x10, u32 sns_offset/0x80, ...
//   SNR (8 bytes): version:4 codec:4 channels-1:6 rate:18 | type:2 loop:1 samples:29
//   SNS blocks: u32 id:8 size:24, u32 samples, then u32 (stream size*4) + XMA packet.
//   One trimmed XMA packet (< 2048 bytes) per block; id 0x80 ends the stream.
// Pure C++17, no rexglue dependency (unit-tested standalone).
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace skate3::custom_music::mus {

constexpr uint32_t kCodecEaXma = 0x03;
constexpr size_t kXmaPacketBytes = 2048;

struct Segment {
  uint64_t snr_offset = 0;
  uint64_t sns_offset = 0;
  uint32_t codec = 0;
  uint32_t channels = 0;
  uint32_t sample_rate = 0;
  uint32_t num_samples = 0;
};

struct Block {
  uint32_t samples = 0;           // samples the block declares
  std::vector<uint8_t> packet;    // trimmed XMA packet
};

// Reads the MUS table and every SNR header.
bool ReadTable(const std::filesystem::path& mus_file, std::vector<Segment>* segments,
               std::string* error);

// Reads the SNS blocks of one segment from an open stream.
bool ReadBlocks(std::istream& in, const Segment& seg, std::vector<Block>* blocks,
                std::string* error);

// Pads the trimmed packets to kXmaPacketBytes each (zero fill), as one contiguous buffer.
std::vector<uint8_t> PadPackets(const std::vector<Block>& blocks);

// Decoder delay of a segment: samples decoded before the first declared sample
// (frames beginning in the first packet * 512 - samples it declares; 384 on Skate 3).
int32_t LeadSamples(const std::vector<Block>& blocks);

// Groups consecutive equal-length segments (+-1 sample) into songs, like the Python tool.
std::vector<std::vector<uint32_t>> GroupSongs(const std::vector<Segment>& segments,
                                              std::vector<uint32_t>* first_segment = nullptr);

// The game's XMA frame decoder outputs a further 192 samples of delay beyond LeadSamples()
// (measured against vgmstream and by seam continuity on all 46 songs).
constexpr int32_t kDecoderExtraDelay = 192;

// Joins the decoded segments of one song (interleaved, `channels` per sample) into the
// song's PCM: each segment contributes num_samples samples from LeadSamples()+192 on. The
// frame decoder cannot flush, so up to ~180 samples at a segment's end can be missing;
// they are filled by linear prediction, and every join is crossfaded over 32 samples from
// a prediction of the previous segment (removes the decoder's cold-start click).
// lead[i] = LeadSamples() of segment i.
std::vector<int16_t> JoinSegments(const std::vector<std::vector<int16_t>>& decoded,
                                  const std::vector<int32_t>& lead,
                                  const std::vector<uint32_t>& num_samples, uint32_t channels);

// Linear-prediction extrapolation of `count` samples after x[0..n) (one channel).
std::vector<double> Extrapolate(const std::vector<double>& x, size_t count);

struct IndexBuildStats {
  size_t packets = 0, unique = 0, duplicates = 0, too_short = 0;
};

// Builds the S3FP v4 fingerprint index from Ipod_Stream.mus (byte-identical to
// `skate3_music_import.py index`). Writes atomically. Progress: segments done / total.
bool BuildIndex(const std::filesystem::path& mus_file, const std::filesystem::path& out_file,
                IndexBuildStats* stats, std::string* error,
                const std::function<bool(size_t, size_t)>& progress = {});

}  // namespace skate3::custom_music::mus
