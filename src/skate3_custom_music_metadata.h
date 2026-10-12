// Skate 3 custom music (dekp23): song info lookups (MusicBrainz + Cover Art Archive).
//
// For your tracks: builds search queries from the file's tags or its name (yt-dlp noise
// removed, "Artist - Title" split both ways), asks the MusicBrainz recording search, and
// accepts a result only when the MusicBrainz score is high and its length agrees with the
// track. The release's front cover (250 px) comes from the Cover Art Archive.
// Results are cached in <library>/metadata.tsv; covers in <library>/covers/.
//
// MusicBrainz rules followed: a descriptive User-Agent and at most one request per second
// (https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting). Lookups are opt-in.
// Pure C++17 apart from the injected HTTP function (unit-tested standalone).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace skate3::custom_music::meta {

constexpr const char* kUserAgent = "dekp23-skate3-custom-music/1.0 ( github.com/dekp23 )";

struct Query {
  std::string artist;  // may be empty
  std::string title;
};

// Search queries for a track, best first: tags (if any), then "A - B" both ways, then the
// whole cleaned name as a title.
std::vector<Query> BuildQueries(const std::string& file_title, const std::string& tag_title,
                                const std::string& tag_artist);
// Removes yt-dlp / upload noise: "(Official Video)", "[Lyrics]", "(feat. X)", "Remastered".
std::string CleanForSearch(const std::string& s);

std::string UrlEncode(const std::string& s);
std::string RecordingSearchUrl(const Query& q, int limit = 10);
std::string CoverUrl(const std::string& release_id);
std::string ReleaseGroupCoverUrl(const std::string& release_group_id);

struct Match {
  std::string title, artist, album;
  std::string recording_id, release_id, release_group_id;
  int score = 0;            // MusicBrainz search score (0..100)
  int64_t length_ms = -1;   // recording length, -1 if unknown
  double confidence = 0;    // ours, 0..1
};

// Parses a /ws/2/recording search response. Candidates are scored against the track
// length (ms, <= 0 = unknown) and returned best first. False on malformed JSON.
bool ParseRecordingSearch(const std::string& json, int64_t track_ms, bool title_only,
                          std::vector<Match>* out);
// The confidence a match needs to be used.
constexpr double kAcceptConfidence = 0.70;

// Cached song info for one playlist entry (key = PlaylistEntry::key()).
struct Info {
  std::string source;  // "musicbrainz", "none" (looked up, nothing found), "manual"
  std::string title, artist, album;
  std::string release_id, recording_id;
  double confidence = 0;
  std::string cover;   // file name inside <library>/covers, or empty
};
std::map<std::string, Info> LoadCache(const std::filesystem::path& library_dir);
bool SaveCache(const std::filesystem::path& library_dir, const std::map<std::string, Info>& cache);

// HTTP GET: returns the status code (or -1 on network error) and fills body.
using HttpGet = std::function<int(const std::string& url, std::string* body)>;

struct LookupInput {
  std::string key;          // playlist key
  std::string file_title;   // title derived from the file name
  std::string tag_title, tag_artist;
  int64_t length_ms = 0;
  std::filesystem::path embedded_cover;  // existing cover file, if the file had one
};

// Looks one track up (several requests, one per second). Returns the info to cache.
// log receives one line per request/decision. cancel stops between requests.
Info Lookup(const HttpGet& get, const LookupInput& in, const std::filesystem::path& covers_dir,
            const std::function<void(const std::string&)>& log,
            const std::atomic<bool>* cancel = nullptr);

// Looks up every track in <library>/tracks.tsv that has no cached info yet (or, with
// retry_missing, whose last lookup found nothing). Saves metadata.tsv after each track.
// progress(done, total, current title). Returns the number of tracks identified.
size_t LookupLibrary(const std::filesystem::path& library_dir, const HttpGet& get, bool retry_missing,
                     const std::function<void(size_t, size_t, const std::string&)>& progress,
                     const std::function<void(const std::string&)>& log,
                     const std::atomic<bool>* cancel);

// Waits so that requests are at least `interval` apart (process-wide).
void RateLimit(std::chrono::milliseconds interval = std::chrono::milliseconds(1100));

}  // namespace skate3::custom_music::meta
