// Skate 3 custom music (dekp23): reading and rewriting the iPod song graph in ipod.mpf
// (EA Pathfinder v5.3, big-endian, as shipped on Xbox 360).
//
// Each iPod song is a chain in section 1 of the node graph:
//   entry node (wave 0) -> segment nodes (wave = sample index + 1) -> end node (wave -1)
// Changing a song's length only changes this chain: extra nodes that replay the song's
// own segments are appended (existing node numbers never change), or the chain is ended
// early. Ipod_Stream.mus is not modified; the audio is replaced at runtime anyway.
// Pure C++17; unit-tested against the real file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skate3::custom_music::mpf {

struct Node {
  uint32_t offset = 0;      // byte offset in the file
  int32_t wave = 0;         // -1 end, 0 entry/null, else sample index + 1
  uint32_t section = 0;
  std::vector<uint16_t> branches;
};

struct Graph {
  std::vector<uint8_t> bytes;  // original file
  uint32_t num_nodes = 0;
  std::vector<Node> nodes;
  // Header offsets (0x14..0x3B, in file order).
  uint32_t node_offsets = 0, node_data = 0, event_offsets = 0, event_data = 0,
           named_vars = 0, node_routers = 0, track_offsets = 0, track_infos = 0,
           sample_offsets = 0, map_file_len = 0;
  uint32_t num_events = 0, num_tracks = 0;
};

struct SongChain {
  uint16_t entry_node = 0;
  uint16_t end_node = 0;
  std::vector<uint16_t> segment_nodes;  // in play order
  std::vector<uint16_t> segments;       // segment index within the song, in play order
};

bool Parse(const std::vector<uint8_t>& data, Graph* out, std::string* error);

// Section-1 chains, one per song. segments_by_song: per-song segment counts in sample
// order (from the fingerprint index). Fails if the graph does not have this shape.
bool ReadChains(const Graph& g, const std::vector<uint32_t>& segments_per_song,
                std::vector<SongChain>* out, std::string* error);

// Rewrites the file so song s plays chains[s] (segment indices; must start with the
// song's first chain segment and contain only that song's segments). Empty chain =
// unchanged. Identity chains reproduce the input byte for byte.
bool WriteWithChains(const Graph& g, const std::vector<uint32_t>& segments_per_song,
                     const std::vector<std::vector<uint16_t>>& chains,
                     std::vector<uint8_t>* out, std::string* error);

}  // namespace skate3::custom_music::mpf
