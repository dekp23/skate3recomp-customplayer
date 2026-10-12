# Skate 3 custom music (dekp23): developer notes

Your own music on the in-game iPod, mixed with the 46 Skate 3 songs in one playlist,
replaced **at the recomp's XMA decoder output**. Game files are never modified. User
documentation: the "Custom Music" section of the [repo README](../../README.md).

## Parts

| Where | What |
|---|---|
| `src/skate3_custom_music.*` | Engine: fingerprints each decoded XMA frame's packet, finds the iPod slot and position, asks the renderer for audio. Trace (diagnostics). |
| `src/skate3_custom_music_player.*` | Playlist player (the renderer): slot position is the clock; anchors map it to (entry, offset). |
| `src/skate3_custom_music_mus.*` | Reads `Ipod_Stream.mus`, builds the fingerprint index in-game, joins decoded segments. |
| `src/skate3_custom_music_mpf.*` | Pathfinder v5.3 (`ipod.mpf`) parser/rewriter for the long-slot ("carrier") layout. |
| `src/skate3_custom_music_library.*` | Track import/conversion, `tracks.tsv`, carrier layout, index. |
| `src/skate3_custom_music_metadata.*` | MusicBrainz + Cover Art Archive lookups, `metadata.tsv`. |
| `src/skate3_custom_music_ui.*` | F7 menu, now-playing pop-up, `player.ini`. |
| `src/skate3_custom_music_glue.*` | rexglue wiring: XMA filter, VFS overlay, in-game decoders, WinHTTP. |
| SDK fork (`custom-music-hook`) | `rex::audio::XmaFrameFilter` hook, `XmaStreamDecoder`, `DecodeAudioFile`, extra FFmpeg components (`thirdparty/ffmpeg-overlay`). |
| This folder | Python inspector/validator/indexer (read-only), test data generators, C++ tests, `windows/run_build.cmd` (one-click Windows build of this branch into `C:\s3build`). |

All `src/skate3_custom_music_*` modules except `glue` and `ui` are plain C++17 with no
rexglue dependency, so they are unit-tested standalone.

## Library folder

`custom_music` next to `skate3.exe` (cvar `skate3_custom_music_dir` overrides; the settings
menu rewrites settings.toml with its own keys only, so nothing is stored there).

| File | Written by | Content |
|---|---|---|
| `ipod.s3fp` | game, first start | fingerprint index (from `Ipod_Stream.mus`, ~0.3 s) |
| `ipod_layout.mpf` | game, first start | `ipod.mpf` with every slot lengthened to >= 19.9 min |
| `game_music/` | game, each start | hard links to the game's music files + the layout as `ipod.mpf`, mounted over `data\audio\music` |
| `tracks/` | you / F7 | your source files |
| `t_<hash>.pcm` (+ `.tags`, `.cover`) | F7 | converted track (44.1 kHz stereo s16le), its tags and embedded picture |
| `tracks.tsv` | F7 | pcm, samples, title, source file, artist, album |
| `playlist.tsv` | F7 | order, on/off, shuffle |
| `metadata.tsv`, `covers/` | F7 (opt-in) | MusicBrainz results and cover art |
| `player.ini` | F7 | on/off, pop-ups, volume, online lookup |

## How it works

- **Engine.** Every iPod XMA packet in `Ipod_Stream.mus` is fingerprinted (FNV-1a 64 of
  packet bytes [4, 260); 93,588 unique, 0 collisions with `Game_Stream.mus`). A decoded frame
  is located as (slot, segment, sample) and, through the slot's chain, as a slot position.
- **Player.** The slot position is the clock. An anchor maps a slot position to (playlist
  entry, offset); a frame is found by walking the playlist from its anchor, so contexts that
  decode ahead (prefetch) get exact audio. A slot that runs out continues the playlist; a
  slot left early (song picked in the game's player) starts the next entry.
- **Long slots.** Every slot's chain is extended to >= 19.9 minutes (8000 extra nodes,
  233 KB `ipod.mpf`) so the game rarely changes song by itself. The rewriter only appends
  nodes; an identity rewrite is byte-identical.
- **The game's songs** are decoded in-game from `Ipod_Stream.mus` with
  `rex::audio::XmaStreamDecoder` (the same FFmpeg `xmaframes` decoder the game uses,
  0.3-1.1 s per song). Audio starts 192 samples after the declared decoder delay (576 in
  total); missing segment tails (<= 180 samples, the frame decoder cannot flush) are
  predicted, and every join is crossfaded over 32 samples.
- **Your tracks** are decoded with `rex::audio::DecodeAudioFile` (FFmpeg 4.4 with extra
  decoders and demuxers) to 44.1 kHz stereo; an installed ffmpeg is only a fallback.
- **Song info** (opt-in): MusicBrainz recording search from tags or the file name, accepted
  only if the score is high and the length agrees; cover from the Cover Art Archive.
  1 request/s, User-Agent `dekp23-skate3-custom-music/1.0 ( github.com/dekp23 )`.
- **Why not re-encode the game files:** EA-XMA is XMA2; the only XMA2 encoder is
  Microsoft's (licensed). FFmpeg's XMA decoders are decode-only.

## Format facts (measured on v2.0.2)

- `ipod.mpf` + `Ipod_Stream.mus`: Pathfinder v5.3, big-endian, 1380 EA-XMA segments
  (44.1 kHz stereo) = 46 songs x 30 equal segments. Section 1 of the node graph holds one
  chain per song (entry -> 30 segment nodes -> end); section 2 holds short versions.
- One trimmed XMA packet (< 2048 bytes) per SNS block; the first packet of a segment
  decodes 384 samples more than it declares.
- Observed in game (trace build): no gap between songs (1-70 ms decoder timing); each ~6 s
  segment is decoded on a fresh XMA context; the game appears to skip a song after ~16 s
  of silence (so the player has no pause).

## Python tool (read-only)

```
py skate3_music_import.py inspect  "...\music\ipod.mpf" [--limit 0] [--json]
py skate3_music_import.py validate "...\music\ipod.mpf"          # exit 0 PASS / 1 errors
py skate3_music_import.py index    "...\music\ipod.mpf" --out ipod.s3fp
py skate3_music_import.py probe    "...\music\ipod.mpf" --subsong 1 --vgmstream PATH
py skate3_music_import.py backup / verify ...
```

## Tests

Configure with `-DSKATE3_CUSTOM_MUSIC_TESTS=ON` (Linux/macOS; tests need no rexglue build)
and run `ctest -R custom_music`. Tests that need the game's music files run only when
`-DSKATE3_TEST_MUSIC_DIR=<game>/data/audio/music` is set (index and replay data are
generated from it by the test fixtures). See `tests/cpp/CMakeLists.txt`.

| Test | Checks |
|---|---|
| `test_player` | playlist mapping with prefetch/lag, slot ran out / left early, next/previous, shuffle, persistence, song info applied |
| `test_metadata` | query building from real file names, scoring, odd JSON types, 503 retry, offline, cache |
| `test_custom_music` | engine replay of real packets: every replaced frame is the renderer's audio at the exact slot position |
| `test_integration` | real packets -> engine -> player, prefetch overlap and an in-game pick, sample-exact |
| `test_library` | converter path and sidecars, ffmpeg fallback, engine controls, carrier layout |
| `test_mpf` | identity rewrite byte-identical, modified graph valid |
| `test_mus` | in-game index byte-identical to the Python index |
| Python `unittest` | MPF/MUS parsers, fingerprints |

Development notes and the in-game test history: [docs/DEVELOPMENT_NOTES.md](docs/DEVELOPMENT_NOTES.md).

Legal: use only your own game copy; don't distribute game files or copyrighted music.
