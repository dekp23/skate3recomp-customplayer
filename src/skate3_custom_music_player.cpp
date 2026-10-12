#include "skate3_custom_music_player.h"

#include "skate3_custom_music_metadata.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace skate3::custom_music {
namespace fs = std::filesystem;

struct Player::Audio {
  std::vector<int16_t> pcm;  // interleaved stereo
  bool failed = false;
};

std::string PlaylistEntry::key() const {
  if (kind == Kind::kOriginal) return "orig:" + std::to_string(original);
  return "track:" + PathToUtf8(pcm.filename());
}

Player::Player() { worker_ = std::thread([this] { WorkerLoop(); }); }

Player::~Player() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void Player::SetOriginalDecoder(OriginalDecoder decoder) {
  std::lock_guard<std::mutex> lock(mutex_);
  decoder_ = std::move(decoder);
}

void Player::SetPlaylist(std::vector<PlaylistEntry> entries) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Keep the entry that is playing, if it is still there.
  std::string playing;
  if (last_entry_ >= 0 && static_cast<size_t>(last_entry_) < entries_.size()) {
    playing = entries_[static_cast<size_t>(last_entry_)].key();
  }
  entries_ = std::move(entries);
  ++version_;
  audio_.clear();
  wanted_.clear();
  int keep = -1;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (!playing.empty() && entries_[i].key() == playing) keep = static_cast<int>(i);
  }
  last_entry_ = keep;
  RebuildOrder(keep, true);
}

std::vector<PlaylistEntry> Player::playlist() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_;
}

void Player::SetShuffle(bool on) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shuffle_ == on) return;
  shuffle_ = on;
  RebuildOrder(last_entry_, true);
}

void Player::SetEnabled(size_t entry, bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (entry >= entries_.size() || entries_[entry].enabled == enabled) return;
  entries_[entry].enabled = enabled;
  RebuildOrder(last_entry_, false);
}

void Player::Move(size_t entry, int delta) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int64_t to = static_cast<int64_t>(entry) + delta;
  if (entry >= entries_.size() || to < 0 || to >= static_cast<int64_t>(entries_.size())) return;
  std::swap(entries_[entry], entries_[static_cast<size_t>(to)]);
  ++version_;
  // Indices of loaded audio and the playing entry follow the swap.
  std::map<size_t, std::shared_ptr<Audio>> moved;
  for (auto& [i, a] : audio_) {
    size_t j = i == entry ? static_cast<size_t>(to) : (i == static_cast<size_t>(to) ? entry : i);
    moved[j] = a;
  }
  audio_.swap(moved);
  wanted_.clear();
  if (last_entry_ == static_cast<int>(entry)) {
    last_entry_ = static_cast<int>(to);
  } else if (last_entry_ == static_cast<int>(to)) {
    last_entry_ = static_cast<int>(entry);
  }
  for (size_t& o : order_) {
    if (o == entry) {
      o = static_cast<size_t>(to);
    } else if (o == static_cast<size_t>(to)) {
      o = entry;
    }
  }
  RebuildOrder(last_entry_, false);
}

size_t Player::Wrap(int64_t order_pos) const {
  const int64_t n = static_cast<int64_t>(order_.size());
  if (n == 0) return 0;
  return static_cast<size_t>(((order_pos % n) + n) % n);
}

int64_t Player::EntryLength(size_t order_pos) const {
  return entries_[order_[order_pos]].samples;
}

void Player::RebuildOrder(int keep_entry, bool reshuffle) {
  const std::vector<size_t> old_order = order_;
  order_.clear();
  auto playable = [&](size_t i) {
    return i < entries_.size() && entries_[i].enabled && entries_[i].samples > 0;
  };
  if (shuffle_ && !reshuffle && !old_order.empty()) {
    // Keep the shuffled order; drop disabled entries, add newly enabled ones at random.
    std::vector<bool> seen(entries_.size(), false);
    for (size_t i : old_order) {
      if (playable(i) && !seen[i]) {
        order_.push_back(i);
        seen[i] = true;
      }
    }
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (playable(i) && !seen[i]) {
        std::uniform_int_distribution<size_t> at(0, order_.size());
        order_.insert(order_.begin() + static_cast<std::ptrdiff_t>(at(rng_)), i);
      }
    }
  } else {
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (playable(i)) order_.push_back(i);
    }
  }
  if (shuffle_ && reshuffle && order_.size() > 1) {
    std::shuffle(order_.begin(), order_.end(), rng_);
    // The playing entry goes first, so play continues from it.
    auto it = std::find(order_.begin(), order_.end(), static_cast<size_t>(std::max(keep_entry, 0)));
    if (keep_entry >= 0 && it != order_.end()) std::iter_swap(order_.begin(), it);
  }
  if (order_.empty()) {
    anchors_.clear();
    return;
  }
  // Re-anchor the playing slot so the current entry keeps playing (or the next one, if
  // it was removed), continuing from the last rendered position.
  size_t pos = 0;
  int64_t offset = 0;
  auto it = std::find(order_.begin(), order_.end(), static_cast<size_t>(std::max(keep_entry, 0)));
  if (keep_entry >= 0 && it != order_.end()) {
    pos = static_cast<size_t>(it - order_.begin());
    offset = last_offset_ + kSamplesPerFrame;
  } else {
    pos = Wrap(static_cast<int64_t>(last_order_pos_));  // next entry now sits here
  }
  start_order_pos_ = pos;
  if (last_song_ >= 0) Reanchor(last_song_, last_pos_ + kSamplesPerFrame, pos, offset);
  last_order_pos_ = pos;
  // Have the first entries ready before the iPod starts.
  Want(order_[pos]);
  Want(order_[Wrap(static_cast<int64_t>(pos) + 1)]);
}

void Player::Reanchor(int32_t song, int64_t slot_pos, size_t order_pos, int64_t offset) {
  auto& list = anchors_[song];
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const Anchor& a) { return a.slot_pos >= slot_pos; }),
             list.end());
  list.push_back({slot_pos, order_pos, offset});
  if (list.size() > 8) list.erase(list.begin());
}

Player::Located Player::Locate(int32_t song, int64_t pos) const {
  Located r;
  auto it = anchors_.find(song);
  if (order_.empty() || it == anchors_.end() || it->second.empty()) return r;
  const Anchor* a = &it->second.front();
  for (const auto& c : it->second) {
    if (c.slot_pos <= pos) a = &c;
  }
  int64_t off = a->offset + (pos - a->slot_pos);
  size_t op = std::min(a->order_pos, order_.size() - 1);
  // Walk forward over whole entries (bounded: one lap of a long playlist at most).
  for (size_t guard = 0; off >= EntryLength(op) && guard < order_.size() * 64; ++guard) {
    off -= EntryLength(op);
    op = Wrap(static_cast<int64_t>(op) + 1);
  }
  r.ok = true;
  r.order_pos = op;
  r.offset = off;
  return r;
}

void Player::OnSlotStart(const SlotEvent& e) {
  std::lock_guard<std::mutex> lock(mutex_);
  current_song_ = e.song;
  if (order_.empty()) return;
  anchors_[e.song].clear();
  size_t op;
  int64_t offset = 0;
  if (e.from_song < 0) {
    op = start_order_pos_;  // first slot of the session
  } else if (!e.restart && e.from_length - e.from_pos <= kNaturalEndWindow) {
    // The game's slot ran out by itself: carry on with what was playing.
    const Located l = last_song_ == e.from_song ? Located{true, last_order_pos_, last_offset_}
                                                : Locate(e.from_song, e.from_pos);
    op = l.ok ? l.order_pos : start_order_pos_;
    offset = l.ok ? l.offset + kSamplesPerFrame : 0;
  } else {
    // A song was picked in the game's music player: next entry.
    op = Wrap(static_cast<int64_t>(last_order_pos_) + 1);
  }
  Reanchor(e.song, e.pos, op, offset);
  Want(order_[op]);
}

void Player::Next() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (order_.empty()) return;
  const size_t op = Wrap(static_cast<int64_t>(last_order_pos_) + 1);
  start_order_pos_ = op;
  if (last_song_ >= 0) Reanchor(last_song_, last_pos_ + kSamplesPerFrame, op, 0);
  Want(order_[op]);
}

void Player::Previous() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (order_.empty()) return;
  // Restart the entry if it has played for more than 3 seconds, else go back one.
  const size_t op = last_offset_ > 3 * 44100 ? last_order_pos_
                                              : Wrap(static_cast<int64_t>(last_order_pos_) - 1);
  start_order_pos_ = op;
  if (last_song_ >= 0) Reanchor(last_song_, last_pos_ + kSamplesPerFrame, op, 0);
  Want(order_[op]);
}

void Player::PlayEntry(size_t entry) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find(order_.begin(), order_.end(), entry);
  if (it == order_.end()) return;
  const size_t op = static_cast<size_t>(it - order_.begin());
  start_order_pos_ = op;
  if (last_song_ >= 0) Reanchor(last_song_, last_pos_ + kSamplesPerFrame, op, 0);
  Want(entry);
}

void Player::Want(size_t entry) {
  if (audio_.count(entry) ||
      std::find(wanted_.begin(), wanted_.end(), entry) != wanted_.end()) {
    return;
  }
  wanted_.push_back(entry);
  cv_.notify_all();
}

bool Player::ReadSamples(size_t entry, int64_t offset, int16_t* stereo) {
  auto it = audio_.find(entry);
  if (it == audio_.end() || it->second->failed) return false;
  const auto& pcm = it->second->pcm;
  const int64_t i = offset * 2;
  if (offset < 0 || i + 1 >= static_cast<int64_t>(pcm.size())) return false;
  stereo[0] = pcm[static_cast<size_t>(i)];
  stereo[1] = pcm[static_cast<size_t>(i + 1)];
  return true;
}

bool Player::Render(int32_t song, int64_t pos, int64_t /*slot_length*/, int16_t* stereo) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (order_.empty()) return false;
  if (anchors_.find(song) == anchors_.end()) {
    // Slot playing before any start event was seen (e.g. player attached mid-song).
    Reanchor(song, pos, start_order_pos_, 0);
  }
  const Located l = Locate(song, pos);
  if (!l.ok) return false;
  size_t op = l.order_pos;
  int64_t off = l.offset;
  std::memset(stereo, 0, kSamplesPerFrame * 2 * sizeof(int16_t));
  for (uint32_t i = 0; i < kSamplesPerFrame; ++i, ++off) {
    if (off >= EntryLength(op)) {
      off = 0;
      op = Wrap(static_cast<int64_t>(op) + 1);
    }
    if (off >= 0) ReadSamples(order_[op], off, stereo + 2 * i);
  }

  // Bookkeeping for status, continuation and prefetch: the furthest frame of the slot.
  if (song != last_song_ || pos >= last_pos_) {
    if (static_cast<int>(order_[l.order_pos]) != last_entry_) ++serial_;
    last_song_ = song;
    last_pos_ = pos;
    last_order_pos_ = l.order_pos;
    last_offset_ = l.offset;
    last_entry_ = static_cast<int>(order_[l.order_pos]);
    last_time_ = Clock::now();
    Want(order_[l.order_pos]);
    Want(order_[Wrap(static_cast<int64_t>(l.order_pos) + 1)]);
  }
  return true;
}

PlayerStatus Player::Status(Clock::time_point now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  PlayerStatus s;
  s.serial = serial_;
  s.shuffle = shuffle_;
  s.entry = last_entry_;
  s.active = last_song_ >= 0 && now - last_time_ < std::chrono::milliseconds(1500);
  if (last_entry_ >= 0 && static_cast<size_t>(last_entry_) < entries_.size()) {
    const auto& e = entries_[static_cast<size_t>(last_entry_)];
    s.title = e.title;
    s.artist = e.artist;
    s.position_seconds = std::max<int64_t>(0, last_offset_) / kSampleRate;
    s.length_seconds = e.samples / kSampleRate;
  }
  return s;
}

void Player::WaitIdle() {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [&] { return stop_ || (wanted_.empty() && !busy_); });
}

void Player::WorkerLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    cv_.wait(lock, [&] { return stop_ || !wanted_.empty(); });
    if (stop_) return;
    const size_t entry = wanted_.front();
    wanted_.erase(wanted_.begin());
    if (entry >= entries_.size() || audio_.count(entry)) continue;
    const PlaylistEntry e = entries_[entry];
    const OriginalDecoder decoder = decoder_;
    const uint64_t generation = version_;
    busy_ = true;
    lock.unlock();

    auto audio = std::make_shared<Audio>();
    if (e.kind == PlaylistEntry::Kind::kOriginal) {
      audio->failed = !decoder || !decoder(e.original, &audio->pcm);
    } else {
      std::ifstream f(e.pcm, std::ios::binary);
      f.seekg(0, std::ios::end);
      const std::streamoff size = f.tellg();
      f.seekg(0);
      if (!f || size < 4) {
        audio->failed = true;
      } else {
        std::vector<uint8_t> raw(static_cast<size_t>(size));
        audio->failed = !f.read(reinterpret_cast<char*>(raw.data()), size);
        audio->pcm.resize(raw.size() / 2);
        for (size_t k = 0; k < audio->pcm.size(); ++k) {
          audio->pcm[k] = static_cast<int16_t>(raw[2 * k] | (raw[2 * k + 1] << 8));
        }
      }
    }

    lock.lock();
    busy_ = false;
    // Drop the result if the playlist was replaced meanwhile.
    const bool same = generation == version_ && entry < entries_.size();
    if (same) audio_[entry] = audio;
    // Keep only the previous, current and next entries in memory.
    if (!order_.empty()) {
      std::set<size_t> keep = {order_[Wrap(static_cast<int64_t>(last_order_pos_) - 1)],
                               order_[Wrap(static_cast<int64_t>(last_order_pos_))],
                               order_[Wrap(static_cast<int64_t>(last_order_pos_) + 1)]};
      for (size_t w : wanted_) keep.insert(w);
      if (same) keep.insert(entry);
      for (auto it = audio_.begin(); it != audio_.end();) {
        it = keep.count(it->first) ? std::next(it) : audio_.erase(it);
      }
    }
    cv_.notify_all();
  }
}

// ---------------------------------------------------------------- persistence

namespace {
std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> out(1);
  for (char c : line) {
    if (c == '\t') {
      out.emplace_back();
    } else if (c != '\r') {
      out.back().push_back(c);
    }
  }
  return out;
}
}  // namespace

std::vector<PlaylistEntry> LoadPlaylist(const fs::path& library_dir,
                                        const std::vector<uint32_t>& original_samples) {
  std::vector<PlaylistEntry> all;
  for (size_t i = 0; i < original_samples.size(); ++i) {
    PlaylistEntry e;
    e.kind = PlaylistEntry::Kind::kOriginal;
    e.original = static_cast<uint32_t>(i);
    e.samples = original_samples[i];
    e.title = "Skate 3 song " + std::to_string(i + 1);
    all.push_back(e);
  }
  // Your tracks: tracks.tsv rows "pcm<TAB>samples<TAB>title<TAB>source".
  {
    std::ifstream in(library_dir / "tracks.tsv");
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      const auto f = SplitTabs(line);
      if (f.size() < 3 || f[0] == "s3tracks") continue;
      PlaylistEntry e;
      e.kind = PlaylistEntry::Kind::kTrack;
      e.pcm = library_dir / PathFromUtf8(f[0]);
      e.samples = std::atoll(f[1].c_str());
      e.title = f[2];
      if (f.size() > 4) e.artist = f[4];
      if (f.size() > 5) e.album = f[5];
      if (e.samples > 0) all.push_back(e);
    }
  }
  // Song info: the file's own tags win; otherwise MusicBrainz results (metadata.tsv).
  {
    const auto cache = meta::LoadCache(library_dir);
    std::error_code ec;
    for (auto& e : all) {
      if (e.kind == PlaylistEntry::Kind::kTrack) {
        const fs::path embedded = fs::path(e.pcm).concat(".cover");
        if (fs::is_regular_file(embedded, ec)) e.cover = embedded;
      }
      const auto it = cache.find(e.key());
      if (it == cache.end()) continue;
      const meta::Info& i = it->second;
      if (i.source != "musicbrainz" && i.source != "manual") continue;
      const bool has_tags = !e.artist.empty();
      if (!has_tags || i.source == "manual") {
        if (!i.title.empty()) e.title = i.title;
        if (!i.artist.empty()) e.artist = i.artist;
        if (!i.album.empty()) e.album = i.album;
      }
      if (e.cover.empty() && !i.cover.empty()) e.cover = library_dir / "covers" / i.cover;
    }
  }
  // Saved order and on/off: playlist.tsv rows "key<TAB>0|1". New entries go at the end.
  std::vector<PlaylistEntry> out;
  std::vector<bool> used(all.size(), false);
  {
    std::ifstream in(library_dir / "playlist.tsv");
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      const auto f = SplitTabs(line);
      if (f.size() < 2) continue;
      for (size_t i = 0; i < all.size(); ++i) {
        if (!used[i] && all[i].key() == f[0]) {
          used[i] = true;
          all[i].enabled = f[1] != "0";
          out.push_back(all[i]);
          break;
        }
      }
    }
  }
  for (size_t i = 0; i < all.size(); ++i) {
    if (!used[i]) out.push_back(all[i]);
  }
  return out;
}

bool SavePlaylist(const fs::path& library_dir, const std::vector<PlaylistEntry>& entries,
                  bool shuffle) {
  const fs::path tmp = library_dir / "playlist.tsv.tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << "# dekp23 custom music playlist: key<TAB>enabled\n";
    out << "#shuffle\t" << (shuffle ? 1 : 0) << "\n";
    for (const auto& e : entries) out << e.key() << '\t' << (e.enabled ? 1 : 0) << '\n';
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(tmp, library_dir / "playlist.tsv", ec);
  return !ec;
}

Player& GlobalPlayer() {
  static Player player;
  return player;
}

bool LoadShuffle(const fs::path& library_dir) {
  std::ifstream in(library_dir / "playlist.tsv");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("#shuffle\t", 0) == 0) return line.size() > 9 && line[9] == '1';
  }
  return false;
}

}  // namespace skate3::custom_music
