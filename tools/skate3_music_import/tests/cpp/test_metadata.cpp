// Tests for src/skate3_custom_music_metadata.cpp (query building, MusicBrainz response
// scoring, cache, lookup flow with a fake HTTP client).
// Build: c++ -std=c++17 -Isrc -I<sdk>/thirdparty/inja/third_party/include
//            src/skate3_custom_music.cpp src/skate3_custom_music_metadata.cpp test_metadata.cpp
#include "skate3_custom_music_metadata.h"

#include <iostream>

using namespace skate3::custom_music::meta;
static int g_failures = 0;
#define CHECK(c, m) do { if (!(c)) { std::cerr << "FAIL: " << m << "\n"; ++g_failures; } } while (0)

static std::string Show(const std::vector<Query>& qs) {
  std::string s;
  for (auto& q : qs) s += "[" + q.artist + " | " + q.title + "] ";
  return s;
}

// A MusicBrainz-shaped response (fields as documented for /ws/2/recording?fmt=json).
static const char* kZedd = R"JSON({"created":"2026-10-12T00:00:00.000Z","count":3,"offset":0,"recordings":[
 {"id":"rec-live","score":100,"title":"Clarity","length":312000,
  "artist-credit":[{"name":"Zedd","joinphrase":" feat. "},{"name":"Foxes","joinphrase":""}],
  "releases":[{"id":"rel-live","title":"Live at X","status":"Official","date":"2014",
   "release-group":{"id":"rg-live","primary-type":"Album","secondary-types":["Live"]}}]},
 {"id":"rec-orig","score":98,"title":"Clarity","length":271000,
  "artist-credit":[{"name":"Zedd","joinphrase":" feat. "},{"name":"Foxes","joinphrase":""}],
  "releases":[{"id":"rel-comp","title":"Hits 2013","status":"Official","date":"2013-05-01",
     "release-group":{"id":"rg-comp","primary-type":"Album","secondary-types":["Compilation"]}},
   {"id":"rel-album","title":"Clarity","status":"Official","date":"2012-10-02",
     "release-group":{"id":"rg-album","primary-type":"Album"}}]},
 {"id":"rec-other","score":60,"title":"Clarity (remix)","length":272000,
  "artist-credit":[{"name":"Someone","joinphrase":""}],"releases":[]}]})JSON";

int main() {
  // ---- query building from the user's actual file names
  auto q1 = BuildQueries("All Of The Lights (Album Version (Edited))", "", "");
  auto q2 = BuildQueries("Ending Time - the Lazarus Plot", "", "");
  auto q3 = BuildQueries("Funeral Diner - I Was The Sword (2024 Remaster)", "", "");
  auto q4 = BuildQueries("Zedd - Clarity (feat. Foxes)", "", "");
  auto q5 = BuildQueries("Se Encendio el Beeper", "", "");
  auto q6 = BuildQueries("whatever", "Real Title", "Real Artist");
  for (auto* q : {&q1, &q2, &q3, &q4, &q5, &q6}) std::cout << Show(*q) << "\n";
  CHECK(q1.size() == 1 && q1[0].title == "All Of The Lights" && q1[0].artist.empty(), "noise removed, title only");
  CHECK(q2.size() == 3 && q2[0].artist == "Ending Time" && q2[1].artist == "the Lazarus Plot", "both orders tried");
  CHECK(q3[0].artist == "Funeral Diner" && q3[0].title == "I Was The Sword", "remaster tag removed");
  CHECK(q4[0].artist == "Zedd" && q4[0].title == "Clarity", "feat. removed");
  CHECK(q6[0].artist == "Real Artist" && q6[0].title == "Real Title", "tags first");
  CHECK(CleanForSearch("Song (Live at Wembley)") == "Song Live at Wembley", "non-noise brackets kept");
  CHECK(CleanForSearch("Song [Official Music Video] ft. Guy") == "Song", "noise + ft removed");

  // ---- URL
  const std::string url = RecordingSearchUrl({"Zedd", "Clarity"});
  std::cout << url << "\n";
  CHECK(url == "https://musicbrainz.org/ws/2/recording/?query=recording%3A%22Clarity%22%20AND%20artist%3A%22Zedd%22&fmt=json&limit=10",
        "search URL");
  CHECK(RecordingSearchUrl({"", "AC/DC: Back"}).find("AC%5C%2FDC%5C%3A") != std::string::npos, "lucene escaping");

  // ---- parsing + scoring: the 271 s original beats the 312 s live version
  std::vector<Match> m;
  CHECK(ParseRecordingSearch(kZedd, 272000, false, &m) && m.size() == 3, "parse");
  CHECK(m[0].recording_id == "rec-orig", "length decides between equal-score results");
  CHECK(m[0].album == "Clarity" && m[0].release_id == "rel-album", "original album chosen over compilation");
  CHECK(m[0].artist == "Zedd feat. Foxes", "artist credit joined");
  CHECK(m[0].confidence >= kAcceptConfidence, "accepted");
  CHECK(ParseRecordingSearch(kZedd, 200000, false, &m) && m[0].confidence < kAcceptConfidence,
        "wrong length is not accepted");
  CHECK(!ParseRecordingSearch("not json", 0, false, &m), "bad json rejected");
  // Unexpected types must not throw (the game may run without exceptions).
  CHECK(ParseRecordingSearch(R"JSON({"recordings":[{"id":null,"title":5,"score":"97","length":"200000",
     "artist-credit":[{"name":null},3],"releases":[1,{"id":"r","status":null,"release-group":"x"}]}, 7]})JSON",
     200000, false, &m) && m.size() == 1 && m[0].score == 97 && m[0].length_ms == 200000, "odd types tolerated");
  CHECK(ParseRecordingSearch(R"({"recordings":[]})", 0, false, &m) && m.empty(), "no results");

  // ---- lookup flow with a fake server
  const auto dir = std::filesystem::temp_directory_path() / "s3meta_test";
  std::filesystem::remove_all(dir);
  std::vector<std::string> urls;
  HttpGet fake = [&](const std::string& u, std::string* body) {
    urls.push_back(u);
    if (u.find("musicbrainz.org") != std::string::npos) {
      if (u.find("Zedd") != std::string::npos) {
        *body = kZedd;
        return 200;
      }
      *body = R"({"recordings":[]})";
      return 200;
    }
    if (u.find("/release/rel-album/") != std::string::npos) return 404;
    if (u.find("/release-group/rg-album/") != std::string::npos) {
      *body = std::string(3000, 'J');
      return 200;
    }
    return 404;
  };
  LookupInput in;
  in.key = "track:t_abc.pcm";
  in.file_title = "Zedd - Clarity (feat. Foxes)";
  in.length_ms = 271500;
  std::vector<std::string> log;
  Info info = Lookup(fake, in, dir / "covers", [&](const std::string& s) { log.push_back(s); });
  for (auto& l : log) std::cout << "  " << l << "\n";
  CHECK(info.source == "musicbrainz" && info.title == "Clarity" && info.artist == "Zedd feat. Foxes", "lookup result");
  CHECK(!info.cover.empty() && std::filesystem::file_size(dir / "covers" / info.cover) == 3000, "cover from release group");
  CHECK(urls.size() == 3, "one search + release cover + release-group cover");

  LookupInput nothing;
  nothing.key = "track:x.pcm";
  nothing.file_title = "Unknown Thing";
  nothing.length_ms = 100000;
  Info none = Lookup(fake, nothing, dir / "covers", nullptr);
  CHECK(none.source == "none" && none.title.empty(), "not found is remembered as none");

  // offline: stops after the first failure
  size_t calls = 0;
  HttpGet offline = [&](const std::string&, std::string*) { ++calls; return -1; };
  Lookup(offline, in, dir / "covers", nullptr);
  CHECK(calls == 1, "offline stops immediately");

  // ---- cache round trip
  std::map<std::string, Info> cache{{in.key, info}, {nothing.key, none}};
  std::filesystem::create_directories(dir);
  CHECK(SaveCache(dir, cache), "save");
  auto back = LoadCache(dir);
  CHECK(back.size() == 2 && back[in.key].artist == "Zedd feat. Foxes" && back[in.key].cover == info.cover &&
            back[nothing.key].source == "none", "cache round trip");

  if (g_failures) { std::cerr << g_failures << " failure(s)\n"; return 1; }
  std::cout << "all checks passed\n";
  return 0;
}
