#include "skate3_custom_music_mpf.h"

#include <cstring>
#include <map>

namespace skate3::custom_music::mpf {
namespace {

uint16_t Be16(const std::vector<uint8_t>& d, size_t o) {
  return static_cast<uint16_t>(d[o] << 8 | d[o + 1]);
}
uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return static_cast<uint32_t>(d[o]) << 24 | static_cast<uint32_t>(d[o + 1]) << 16 |
         static_cast<uint32_t>(d[o + 2]) << 8 | d[o + 3];
}
void Put16(std::vector<uint8_t>& d, size_t o, uint32_t v) {
  d[o] = static_cast<uint8_t>(v >> 8);
  d[o + 1] = static_cast<uint8_t>(v);
}
void Put32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
  d[o] = static_cast<uint8_t>(v >> 24);
  d[o + 1] = static_cast<uint8_t>(v >> 16);
  d[o + 2] = static_cast<uint8_t>(v >> 8);
  d[o + 3] = static_cast<uint8_t>(v);
}

constexpr size_t kHeaderSize = 0x48;
constexpr size_t kNodeFixed = 16;  // 3 bitfield words + extra word
constexpr uint32_t kMaxWordOffset = 0xFFFF * 4;

bool Fail(std::string* e, const std::string& m) {
  if (e) *e = m;
  return false;
}

}  // namespace

bool Parse(const std::vector<uint8_t>& d, Graph* g, std::string* error) {
  if (d.size() < kHeaderSize || std::memcmp(d.data(), "PFDx", 4) != 0) {
    return Fail(error, "not a big-endian Pathfinder file");
  }
  if (d[4] != 5 || d[5] != 3) return Fail(error, "expected Pathfinder v5.3");
  g->bytes = d;
  g->num_tracks = d[0x0D];
  g->num_events = d[0x0F];
  g->num_nodes = Be16(d, 0x12);
  uint32_t* fields[] = {&g->node_offsets,  &g->node_data,   &g->event_offsets, &g->event_data,
                        &g->named_vars,    &g->node_routers, &g->track_offsets, &g->track_infos,
                        &g->sample_offsets, &g->map_file_len};
  for (size_t i = 0; i < 10; ++i) {
    *fields[i] = Be32(d, 0x14 + 4 * i);
    if (*fields[i] > d.size()) return Fail(error, "header offset beyond file");
  }
  if (g->map_file_len != d.size()) return Fail(error, "mapfilelen != file size");
  if (g->node_offsets != kHeaderSize) return Fail(error, "unexpected node table position");
  g->nodes.resize(g->num_nodes);
  for (uint32_t i = 0; i < g->num_nodes; ++i) {
    Node& n = g->nodes[i];
    n.offset = Be16(d, g->node_offsets + 2 * i) * 4u;
    if (n.offset + kNodeFixed > d.size()) return Fail(error, "node offset beyond file");
    const uint32_t w0 = Be32(d, n.offset), w1 = Be32(d, n.offset + 4);
    n.wave = static_cast<int16_t>(w0 >> 16);
    n.section = (w0 >> 5) & 63;
    const uint32_t nbr = (w1 >> 15) & 31;
    if (n.offset + kNodeFixed + 4 * nbr > d.size()) return Fail(error, "node branches beyond file");
    for (uint32_t b = 0; b < nbr; ++b) {
      n.branches.push_back(Be16(d, n.offset + kNodeFixed + 4 * b + 2));
    }
  }
  // Layout this writer relies on: nodes packed in order, directly followed by the
  // event offset table.
  uint32_t expect = g->node_data;
  for (const Node& n : g->nodes) {
    if (n.offset != expect) return Fail(error, "nodes are not packed in order");
    expect += kNodeFixed + 4 * static_cast<uint32_t>(n.branches.size());
  }
  // Up to 15 zero bytes of alignment padding may follow the nodes (WriteWithChains).
  if (expect > g->event_offsets || g->event_offsets - expect > 15) {
    return Fail(error, "node data does not end at the event table");
  }
  for (uint32_t i = expect; i < g->event_offsets; ++i) {
    if (d[i] != 0) return Fail(error, "unexpected bytes between nodes and event table");
  }
  return true;
}

bool ReadChains(const Graph& g, const std::vector<uint32_t>& segments_per_song,
                std::vector<SongChain>* out, std::string* error) {
  // wave -> (song, segment)
  std::vector<std::pair<uint32_t, uint16_t>> wave_map{{0, 0}};  // index 0 unused
  for (uint32_t s = 0; s < segments_per_song.size(); ++s) {
    for (uint32_t k = 0; k < segments_per_song[s]; ++k) {
      wave_map.push_back({s, static_cast<uint16_t>(k)});
    }
  }
  out->assign(segments_per_song.size(), SongChain{});
  std::vector<bool> found(segments_per_song.size(), false);
  for (uint32_t i = 0; i < g.num_nodes; ++i) {
    const Node& entry = g.nodes[i];
    if (entry.section != 1 || entry.wave != 0 || entry.branches.size() != 1) continue;
    SongChain chain;
    chain.entry_node = static_cast<uint16_t>(i);
    int32_t song = -1;
    uint32_t cur = entry.branches[0];
    for (size_t steps = 0;; ++steps) {
      if (cur >= g.num_nodes || steps > 100000) return Fail(error, "broken chain");
      const Node& n = g.nodes[cur];
      if (n.wave == -1) {
        chain.end_node = static_cast<uint16_t>(cur);
        break;
      }
      if (n.wave <= 0 || static_cast<size_t>(n.wave) >= wave_map.size() || n.branches.size() != 1) {
        return Fail(error, "unexpected node in song chain");
      }
      const auto [s, k] = wave_map[static_cast<size_t>(n.wave)];
      if (song < 0) song = static_cast<int32_t>(s);
      if (static_cast<uint32_t>(song) != s) return Fail(error, "chain mixes songs");
      chain.segment_nodes.push_back(static_cast<uint16_t>(cur));
      chain.segments.push_back(k);
      cur = n.branches[0];
    }
    if (song < 0) return Fail(error, "empty song chain");
    if (found[static_cast<size_t>(song)]) return Fail(error, "two section-1 chains for one song");
    found[static_cast<size_t>(song)] = true;
    (*out)[static_cast<size_t>(song)] = std::move(chain);
  }
  for (size_t s = 0; s < found.size(); ++s) {
    if (!found[s]) return Fail(error, "no section-1 chain for song " + std::to_string(s));
  }
  return true;
}

bool WriteWithChains(const Graph& g, const std::vector<uint32_t>& segments_per_song,
                     const std::vector<std::vector<uint16_t>>& chains, std::vector<uint8_t>* out,
                     std::string* error) {
  std::vector<SongChain> current;
  if (!ReadChains(g, segments_per_song, &current, error)) return false;

  // Node bytes (copied) and per-node branch targets, plus appended clones.
  struct OutNode {
    uint32_t src_offset;               // original node to copy
    std::vector<uint16_t> branches;
  };
  std::vector<OutNode> nodes;
  nodes.reserve(g.num_nodes);
  for (const Node& n : g.nodes) nodes.push_back({n.offset, n.branches});

  for (size_t s = 0; s < chains.size() && s < current.size(); ++s) {
    const auto& want = chains[s];
    if (want.empty()) continue;
    const SongChain& cur = current[s];
    // Original node for each segment of the song (from the current chain).
    std::map<uint16_t, uint16_t> node_for_segment;
    for (size_t i = 0; i < cur.segments.size(); ++i) {
      node_for_segment.emplace(cur.segments[i], cur.segment_nodes[i]);
    }
    if (want.front() != cur.segments.front()) {
      return Fail(error, "chain for song " + std::to_string(s) + " must start with its first segment");
    }
    std::vector<bool> used(g.num_nodes, false);
    std::vector<uint16_t> path;
    for (uint16_t seg : want) {
      auto it = node_for_segment.find(seg);
      if (it == node_for_segment.end()) {
        return Fail(error, "segment " + std::to_string(seg) + " not in song " + std::to_string(s));
      }
      if (!used[it->second]) {
        used[it->second] = true;
        path.push_back(it->second);
      } else {
        if (nodes.size() >= 0xFFFF) return Fail(error, "too many nodes");
        nodes.push_back({g.nodes[it->second].offset, {0}});
        path.push_back(static_cast<uint16_t>(nodes.size() - 1));
      }
    }
    nodes[cur.entry_node].branches = {path.front()};
    for (size_t i = 0; i + 1 < path.size(); ++i) nodes[path[i]].branches = {path[i + 1]};
    nodes[path.back()].branches = {cur.end_node};
  }

  // ---- serialize
  const uint32_t n = static_cast<uint32_t>(nodes.size());
  uint32_t node_data = static_cast<uint32_t>(kHeaderSize + 2 * n);
  node_data = (node_data + 3) & ~3u;
  std::vector<uint8_t> o(g.bytes.begin(), g.bytes.begin() + kHeaderSize);
  o.resize(node_data, 0);
  std::vector<uint32_t> offsets;
  for (const OutNode& node : nodes) {
    const uint32_t off = static_cast<uint32_t>(o.size());
    if (off > kMaxWordOffset) return Fail(error, "node data exceeds the 16-bit offset range");
    offsets.push_back(off);
    o.insert(o.end(), g.bytes.begin() + node.src_offset,
             g.bytes.begin() + node.src_offset + kNodeFixed);
    // branch count lives in word 1 bits 19..15
    uint32_t w1 = Be32(o, off + 4);
    w1 = (w1 & ~(31u << 15)) | (static_cast<uint32_t>(node.branches.size()) << 15);
    Put32(o, off + 4, w1);
    const uint32_t src_nbr = (Be32(g.bytes, node.src_offset + 4) >> 15) & 31;
    for (size_t b = 0; b < node.branches.size(); ++b) {
      // Keep the original branch's control range when there is one.
      if (b < src_nbr) {
        const size_t src = node.src_offset + kNodeFixed + 4 * b;
        o.insert(o.end(), g.bytes.begin() + src, g.bytes.begin() + src + 2);
      } else {
        o.push_back(0x00);
        o.push_back(0x7F);
      }
      o.push_back(static_cast<uint8_t>(node.branches[b] >> 8));
      o.push_back(static_cast<uint8_t>(node.branches[b]));
    }
  }
  // Tail (event table .. end) moves by delta, padded so its alignment mod 16 is kept.
  uint32_t tail = static_cast<uint32_t>(o.size());
  while ((tail & 15) != (g.event_offsets & 15)) ++tail;
  o.resize(tail, 0);
  const int64_t delta = static_cast<int64_t>(tail) - g.event_offsets;
  o.insert(o.end(), g.bytes.begin() + g.event_offsets, g.bytes.end());

  for (uint32_t i = 0; i < n; ++i) Put16(o, kHeaderSize + 2 * i, offsets[i] / 4);
  Put16(o, 0x12, n);
  Put32(o, 0x18, node_data);
  const uint32_t moved[] = {g.event_offsets, g.event_data,    g.named_vars,     g.node_routers,
                            g.track_offsets, g.track_infos,   g.sample_offsets, g.map_file_len};
  for (size_t i = 0; i < 8; ++i) {
    Put32(o, 0x1C + 4 * i, static_cast<uint32_t>(moved[i] + delta));
  }
  const uint32_t ev_table = static_cast<uint32_t>(g.event_offsets + delta);
  for (uint32_t i = 0; i < g.num_events; ++i) {
    const uint32_t v = Be16(o, ev_table + 2 * i) * 4u + static_cast<uint32_t>(delta);
    if (v > kMaxWordOffset) return Fail(error, "event offset exceeds 16-bit range");
    Put16(o, ev_table + 2 * i, v / 4);
  }
  const uint32_t tr_table = static_cast<uint32_t>(g.track_offsets + delta);
  for (uint32_t i = 0; i < g.num_tracks; ++i) {
    Put32(o, tr_table + 4 * i, (Be32(o, tr_table + 4 * i) * 4u + static_cast<uint32_t>(delta)) / 4);
  }
  if (static_cast<int64_t>(o.size()) != g.map_file_len + delta) {
    return Fail(error, "internal size mismatch");
  }
  out->swap(o);
  return true;
}

}  // namespace skate3::custom_music::mpf
