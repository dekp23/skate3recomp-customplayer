#include "skate3_custom_music_metadata.h"

#include "skate3_custom_music.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace skate3::custom_music::meta {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(const std::string& s) {
  const char* ws = " \t\r\n-_.,;:|~";
  const auto a = s.find_first_not_of(ws);
  if (a == std::string::npos) return "";
  const auto b = s.find_last_not_of(" \t\r\n-_,;:|~");
  return s.substr(a, b - a + 1);
}

std::string CollapseSpaces(const std::string& s) {
  std::string out;
  bool space = false;
  for (char c : s) {
    if (c == ' ' || c == '\t') {
      space = !out.empty();
    } else {
      if (space) out.push_back(' ');
      space = false;
      out.push_back(c);
    }
  }
  return out;
}

// Words that mark a bracketed part as upload noise rather than part of the title.
bool IsNoise(const std::string& inner) {
  static const char* kWords[] = {"official", "video", "audio", "lyric", "visualizer", "visualiser",
                                 "remaster", "album version", "explicit", "edited", "clean",
                                 "hd", "hq", "4k", "feat", "ft.", "prod", "music video", "mv",
                                 "full song", "with lyrics", "audio only", "single version"};
  const std::string l = Lower(inner);
  for (const char* w : kWords) {
    const std::string word = w;
    size_t p = l.find(word);
    while (p != std::string::npos) {
      const bool left_ok = p == 0 || !std::isalnum(static_cast<unsigned char>(l[p - 1]));
      const size_t e = p + word.size();
      const bool right_ok = e >= l.size() || !std::isalnum(static_cast<unsigned char>(l[e])) ||
                            word == "remaster" || word == "lyric";
      if (left_ok && right_ok) return true;
      p = l.find(word, p + 1);
    }
  }
  return false;
}

// Splits on the first " - " (also en/em dash). Returns false if there is none.
bool SplitDash(const std::string& s, std::string* a, std::string* b) {
  static const char* kSeps[] = {" - ", " \xE2\x80\x93 ", " \xE2\x80\x94 ", " | "};
  for (const char* sep : kSeps) {
    const auto p = s.find(sep);
    if (p != std::string::npos) {
      *a = Trim(s.substr(0, p));
      *b = Trim(s.substr(p + std::string(sep).size()));
      return !a->empty() && !b->empty();
    }
  }
  return false;
}

std::string LuceneEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (std::string("+-&|!(){}[]^\"~*?:\\/").find(c) != std::string::npos) out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

std::string Str(const json& o, const char* key) {
  if (!o.is_object()) return "";
  const auto it = o.find(key);
  return it != o.end() && it->is_string() ? it->get<std::string>() : "";
}

int64_t Int(const json& o, const char* key, int64_t def) {
  if (!o.is_object()) return def;
  const auto it = o.find(key);
  if (it == o.end()) return def;
  if (it->is_number_integer() || it->is_number_unsigned()) return it->get<int64_t>();
  if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
  if (it->is_string()) return std::atoll(it->get<std::string>().c_str());
  return def;
}

std::string ArtistCredit(const json& ac) {
  std::string out;
  if (!ac.is_array()) return out;
  for (const auto& c : ac) {
    out += Str(c, "name");
    out += Str(c, "joinphrase");
  }
  return out;
}

std::string SafeName(const std::string& key) {
  std::string out;
  uint64_t h = 0xCBF29CE484222325ull;
  for (unsigned char c : key) {
    h = (h ^ c) * 0x100000001B3ull;
    out.push_back(std::isalnum(c) ? static_cast<char>(c) : '_');
  }
  if (out.size() > 40) out.resize(40);
  char buf[20];
  std::snprintf(buf, sizeof(buf), "_%08x", static_cast<unsigned>(h & 0xFFFFFFFFu));
  return out + buf;
}

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

std::string NoTabs(std::string v) {
  for (char& c : v) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return v;
}

}  // namespace

std::string CleanForSearch(const std::string& s_in) {
  std::string s = s_in;
  // Bracketed parts: drop the noisy ones, keep the rest without brackets.
  for (int pass = 0; pass < 4; ++pass) {
    bool changed = false;
    for (const auto& [open, close] : {std::pair<char, char>{'(', ')'}, {'[', ']'}, {'{', '}'}}) {
      const auto a = s.find(open);
      if (a == std::string::npos) continue;
      // Innermost-first: find the matching close allowing one nesting level.
      int depth = 0;
      size_t b = std::string::npos;
      for (size_t i = a; i < s.size(); ++i) {
        if (s[i] == open) ++depth;
        if (s[i] == close && --depth == 0) {
          b = i;
          break;
        }
      }
      if (b == std::string::npos) {
        s.erase(a, 1);
      } else {
        const std::string inner = s.substr(a + 1, b - a - 1);
        s.replace(a, b - a + 1, IsNoise(inner) ? " " : " " + inner + " ");
      }
      changed = true;
    }
    if (!changed) break;
  }
  // Unbracketed "feat. X" / "ft. X" up to the end or a dash.
  const std::string l = Lower(s);
  for (const char* f : {" feat. ", " feat ", " ft. ", " featuring "}) {
    const auto p = l.find(f);
    if (p != std::string::npos) {
      const auto dash = s.find(" - ", p);
      s.erase(p, dash == std::string::npos ? std::string::npos : dash - p);
      break;
    }
  }
  return Trim(CollapseSpaces(s));
}

std::vector<Query> BuildQueries(const std::string& file_title, const std::string& tag_title,
                                const std::string& tag_artist) {
  std::vector<Query> out;
  auto add = [&](std::string artist, std::string title) {
    artist = CleanForSearch(artist);
    title = CleanForSearch(title);
    if (title.empty()) return;
    for (const auto& q : out) {
      if (Lower(q.artist) == Lower(artist) && Lower(q.title) == Lower(title)) return;
    }
    out.push_back({artist, title});
  };
  if (!tag_title.empty() && !tag_artist.empty()) add(tag_artist, tag_title);
  for (const std::string& name : {tag_title, file_title}) {
    if (name.empty()) continue;
    std::string a, b;
    if (SplitDash(name, &a, &b)) {
      add(a, b);  // "Artist - Title" (most common)
      add(b, a);  // "Title - Artist"
    }
  }
  add("", tag_title.empty() ? file_title : tag_title);
  if (out.size() > 4) out.resize(4);
  return out;
}

std::string UrlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

std::string RecordingSearchUrl(const Query& q, int limit) {
  std::string lucene = "recording:\"" + LuceneEscape(q.title) + "\"";
  if (!q.artist.empty()) lucene += " AND artist:\"" + LuceneEscape(q.artist) + "\"";
  return "https://musicbrainz.org/ws/2/recording/?query=" + UrlEncode(lucene) +
         "&fmt=json&limit=" + std::to_string(limit);
}

std::string CoverUrl(const std::string& release_id) {
  return "https://coverartarchive.org/release/" + release_id + "/front-250";
}

std::string ReleaseGroupCoverUrl(const std::string& release_group_id) {
  return "https://coverartarchive.org/release-group/" + release_group_id + "/front-250";
}

bool ParseRecordingSearch(const std::string& text, int64_t track_ms, bool title_only,
                          std::vector<Match>* out) {
  out->clear();
  const json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) return false;
  const auto it = doc.find("recordings");
  if (it == doc.end() || !it->is_array()) return true;  // no results
  for (const auto& r : *it) {
    if (!r.is_object()) continue;
    Match m;
    m.recording_id = Str(r, "id");
    m.title = Str(r, "title");
    m.score = static_cast<int>(Int(r, "score", 0));
    m.length_ms = Int(r, "length", -1);
    if (r.contains("artist-credit")) m.artist = ArtistCredit(r["artist-credit"]);
    // Pick the most "original" release: official album, earliest.
    int best = -1000;
    std::string best_date;
    if (r.contains("releases") && r["releases"].is_array()) {
      for (const auto& rel : r["releases"]) {
        if (!rel.is_object()) continue;
        int s = 0;
        if (Str(rel, "status") == "Official") s += 2;
        std::string primary, rgid;
        bool secondary = false;
        if (rel.contains("release-group") && rel["release-group"].is_object()) {
          const auto& rg = rel["release-group"];
          primary = Str(rg, "primary-type");
          rgid = Str(rg, "id");
          secondary = rg.contains("secondary-types") && rg["secondary-types"].is_array() &&
                      !rg["secondary-types"].empty();
        }
        if (primary == "Album") s += 2;
        if (primary == "Single" || primary == "EP") s += 1;
        if (secondary) s -= 3;
        std::string date = Str(rel, "date");
        if (date.empty()) date = "9999";
        if (s > best || (s == best && date < best_date)) {
          best = s;
          best_date = date;
          m.album = Str(rel, "title");
          m.release_id = Str(rel, "id");
          m.release_group_id = rgid;
        }
      }
    }
    // Confidence: MusicBrainz score, weighted by how well the length agrees.
    double length_factor = title_only ? 0.4 : 0.75;  // length unknown
    if (track_ms > 0 && m.length_ms > 0) {
      const double d = std::fabs(static_cast<double>(track_ms - m.length_ms)) / 1000.0;
      if (title_only) {
        length_factor = d <= 3 ? 1.0 : d <= 6 ? 0.6 : 0.1;
      } else {
        length_factor = d <= 3 ? 1.0 : d <= 6 ? 0.95 : d <= 12 ? 0.8 : d <= 25 ? 0.5 : 0.1;
      }
    }
    m.confidence = (m.score / 100.0) * length_factor;
    out->push_back(std::move(m));
  }
  std::stable_sort(out->begin(), out->end(),
                   [](const Match& a, const Match& b) { return a.confidence > b.confidence; });
  return true;
}

std::map<std::string, Info> LoadCache(const fs::path& dir) {
  std::map<std::string, Info> out;
  std::ifstream in(dir / "metadata.tsv");
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto f = SplitTabs(line);
    if (f.size() < 9) continue;
    Info i;
    i.source = f[1];
    i.title = f[2];
    i.artist = f[3];
    i.album = f[4];
    i.release_id = f[5];
    i.recording_id = f[6];
    i.confidence = std::atof(f[7].c_str());
    i.cover = f[8];
    out[f[0]] = i;
  }
  return out;
}

bool SaveCache(const fs::path& dir, const std::map<std::string, Info>& cache) {
  const fs::path tmp = dir / "metadata.tsv.tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << "# dekp23 custom music song info: key source title artist album release recording "
           "confidence cover\n";
    for (const auto& [k, i] : cache) {
      char conf[16];
      std::snprintf(conf, sizeof(conf), "%.3f", i.confidence);
      out << NoTabs(k) << '\t' << NoTabs(i.source) << '\t' << NoTabs(i.title) << '\t'
          << NoTabs(i.artist) << '\t' << NoTabs(i.album) << '\t' << NoTabs(i.release_id) << '\t'
          << NoTabs(i.recording_id) << '\t' << conf << '\t' << NoTabs(i.cover) << '\n';
    }
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(tmp, dir / "metadata.tsv", ec);
  return !ec;
}

void RateLimit(std::chrono::milliseconds interval) {
  static std::mutex mu;
  static std::chrono::steady_clock::time_point last{};
  std::lock_guard<std::mutex> lock(mu);
  const auto now = std::chrono::steady_clock::now();
  if (now - last < interval) std::this_thread::sleep_for(interval - (now - last));
  last = std::chrono::steady_clock::now();
}

Info Lookup(const HttpGet& get, const LookupInput& in, const fs::path& covers_dir,
            const std::function<void(const std::string&)>& log, const std::atomic<bool>* cancel) {
  Info info;
  info.source = "none";
  auto say = [&](const std::string& s) {
    if (log) log(s);
  };
  Match best;
  bool found = false;
  for (const Query& q : BuildQueries(in.file_title, in.tag_title, in.tag_artist)) {
    if (cancel && cancel->load()) return info;
    const std::string url = RecordingSearchUrl(q);
    std::string body;
    int status = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
      RateLimit();
      status = get(url, &body);
      if (status != 503) break;  // MusicBrainz asks clients to slow down with 503
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (status != 200) {
      say("musicbrainz: HTTP " + std::to_string(status) + " for '" + q.artist + " / " + q.title + "'");
      if (status < 0) break;  // offline: stop
      continue;
    }
    std::vector<Match> matches;
    if (!ParseRecordingSearch(body, in.length_ms, q.artist.empty(), &matches)) {
      say("musicbrainz: unreadable response");
      continue;
    }
    if (!matches.empty()) {
      const Match& m = matches.front();
      char line[512];
      std::snprintf(line, sizeof(line),
                    "musicbrainz: '%s / %s' -> '%s - %s' (%s) score %d, length %.0f s vs %.0f s, "
                    "confidence %.2f",
                    q.artist.c_str(), q.title.c_str(), m.artist.c_str(), m.title.c_str(),
                    m.album.c_str(), m.score, m.length_ms / 1000.0, in.length_ms / 1000.0,
                    m.confidence);
      say(line);
      if (m.confidence > best.confidence) best = m;
      if (m.confidence >= kAcceptConfidence) {
        found = true;
        break;
      }
    } else {
      say("musicbrainz: no results for '" + q.artist + " / " + q.title + "'");
    }
  }
  if (!found) return info;

  info.source = "musicbrainz";
  info.title = best.title;
  info.artist = best.artist;
  info.album = best.album;
  info.release_id = best.release_id;
  info.recording_id = best.recording_id;
  info.confidence = best.confidence;

  // Cover: the file's own picture wins; else the Cover Art Archive (release, then group).
  std::error_code ec;
  fs::create_directories(covers_dir, ec);
  const std::string name = SafeName(in.key) + ".jpg";
  if (!in.embedded_cover.empty() && fs::is_regular_file(in.embedded_cover, ec)) {
    fs::copy_file(in.embedded_cover, covers_dir / name, fs::copy_options::overwrite_existing, ec);
    if (!ec) info.cover = name;
  }
  if (info.cover.empty()) {
    for (const std::string& url :
         {best.release_id.empty() ? std::string() : CoverUrl(best.release_id),
          best.release_group_id.empty() ? std::string() : ReleaseGroupCoverUrl(best.release_group_id)}) {
      if (url.empty() || (cancel && cancel->load())) continue;
      std::string body;
      RateLimit(std::chrono::milliseconds(500));
      const int status = get(url, &body);
      if (status == 200 && body.size() > 100) {
        std::ofstream f(covers_dir / name, std::ios::binary | std::ios::trunc);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (f) {
          info.cover = name;
          say("cover art: " + std::to_string(body.size()) + " bytes");
          break;
        }
      } else {
        say("cover art: HTTP " + std::to_string(status));
      }
    }
  }
  return info;
}

size_t LookupLibrary(const fs::path& dir, const HttpGet& get, bool retry_missing,
                     const std::function<void(size_t, size_t, const std::string&)>& progress,
                     const std::function<void(const std::string&)>& log,
                     const std::atomic<bool>* cancel) {
  // tracks.tsv: pcm, samples, title, source, artist, album
  struct Row {
    std::string pcm, title, source;
    int64_t samples = 0;
  };
  std::vector<Row> rows;
  {
    std::ifstream in(dir / "tracks.tsv");
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      const auto f = SplitTabs(line);
      if (f.size() < 4 || f[0] == "s3tracks") continue;
      rows.push_back({f[0], f[2], f[3], std::atoll(f[1].c_str())});
    }
  }
  auto cache = LoadCache(dir);
  std::vector<const Row*> todo;
  for (const auto& r : rows) {
    const auto it = cache.find("track:" + r.pcm);
    if (it == cache.end() || (retry_missing && it->second.source == "none")) todo.push_back(&r);
  }
  size_t found = 0;
  for (size_t i = 0; i < todo.size(); ++i) {
    if (cancel && cancel->load()) break;
    const Row& r = *todo[i];
    if (progress) progress(i, todo.size(), r.title);
    LookupInput in;
    in.key = "track:" + r.pcm;
    // The file name (minus extension and yt-dlp id) carries the most information.
    std::string name = PathToUtf8(PathFromUtf8(r.source).stem());
    const auto br = name.rfind(" [");
    if (br != std::string::npos && name.size() - br == 14 && name.back() == ']') name.resize(br);
    in.file_title = name.empty() ? r.title : name;
    {
      std::ifstream tf(dir / (r.pcm + ".tags"));
      std::string line;
      if (std::getline(tf, line)) {
        const auto f = SplitTabs(line);
        if (f.size() >= 2) in.tag_title = f[0], in.tag_artist = f[1];
      }
    }
    in.length_ms = r.samples * 1000 / 44100;
    std::error_code ec;
    if (fs::is_regular_file(dir / (r.pcm + ".cover"), ec)) in.embedded_cover = dir / (r.pcm + ".cover");
    Info info = Lookup(get, in, dir / "covers", log, cancel);
    if (cancel && cancel->load()) break;
    if (info.source == "musicbrainz") ++found;
    cache[in.key] = info;
    SaveCache(dir, cache);
  }
  if (progress) progress(todo.size(), todo.size(), "");
  return found;
}

}  // namespace skate3::custom_music::meta
