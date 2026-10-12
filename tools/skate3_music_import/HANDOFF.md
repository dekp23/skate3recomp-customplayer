# Skate 3 Custom Music (dekp23): handoff for 2026-10-12

## Where things stand (2026-10-11, end of day)

**Working and confirmed in game**
- Your tracks replace iPod songs at the recomp's XMA decoder output. Game files untouched.
- "NOW PLAYING" pop-up (bottom left, dekp23 branding) and F7 menu: on/off, volume,
  pop-ups, leftover-slot mode, Add tracks / Import Music folder / Rebuild, slot table.
  User feedback: pop-up loved, menu good.

**Built but not yet confirmed in game**
- Song-length fitting (rewritten `ipod.mpf` served through the VFS). Superseded by the
  plan below; keep the code, it becomes the "long carrier slots" mechanism.

**User feedback driving the next step**
- The 46-slot model (cut-offs / trailing silence) should go away entirely.
- Wanted: one playlist containing original game songs *and* your tracks, user picks which
  play. Plus proper title/artist/album and cover art in the pop-up and menu.

## Repos, branches, machines

| What | Where |
|---|---|
| Recomp fork | `dekp23/skate3recomp-customplayer`, branch `custom-music-import` |
| SDK fork | `dekp23/rexglue-skate3-customplayer`, branch `custom-music-hook` (`5df62be`: frame filter hook, imgui pin, XmaStreamDecoder) |
| Build | `C:\s3build\skate3recomp` (script: `...\Skate3Recomp-Windows\custom_music_build\run_build.cmd`, ~10 min incremental) |
| Test install | `C:\s3build\Skate3Recomp-CustomMusic\skate3.exe` (game folder linked, saves copied) |
| Library | `C:\s3build\custom_music` (linked as `custom_music` next to the exe); index is v4 |
| Original install | `C:\Skate3Recomp-Windows\Skate3Recomp-Windows` (never modified) |
| Logs | `C:\s3build\Skate3Recomp-CustomMusic\logs\` |

Toolchain on the laptop: LLVM 23.1.3, CMake 4.4.4, Ninja, VS 2026 Build Tools, Python 3.14, FFmpeg 9.0.2.

## Decisions made (2026-10-11)

- Playlist approach: **both** - our playlist now, research the game's native song list later.
- Metadata sources: **all four** - file tags + embedded covers, MusicBrainz + Cover Art
  Archive (opt-in), AcoustID, manual editing.
- Storage of the 46 originals and your tracks: **FLAC** (~0.8 GB), decoded in-game with
  dr_flac (public domain, single header).

## Plan

### Phase 0 - spikes: DONE 2026-10-12 (results)
- [x] **Covers in ImGui: supported, no SDK change.** rexglue sets
      `ImGuiBackendFlags_RendererHasTextures` and services `ImTextureData` requests through
      the ImmediateDrawer. Headless test at the pinned imgui (6d910d5): a 256x256 texture
      registered with `ImGui::RegisterUserTexture` is created by the backend and drawn
      from the 2nd frame (1st frame is ImGui's normal auto-size hidden frame). Notes:
      API is in imgui_internal.h ("experimental"); before `WantDestroy` set
      `UnusedFrames = 1` (backend waits for it; ImGui only counts it for atlases); after a
      presenter change the backend marks textures `Destroyed` - set `WantCreate` again.
- [x] **Originals decode exactly.** 1380/1380 segments decoded (vgmstream + FFmpeg, 35 s);
      every segment length equals its SNR sample count. Total 2 h 25 min; FLAC ratio 0.62
      measured (lossless round trip byte-exact) -> **~0.96 GB**.
- [x] **Seams:** plain joins click at ~1/3 of joins (first decoded sample of each segment is
      off - cold decoder start; the game itself likely plays this too - inference). A
      32-sample LPC-extrapolated crossfade fixes it: joins above the local 99.9th-percentile
      curvature 427 -> 8 of 1334, worst 9.3x -> 1.8x. Metric only, not listened to.
      Script: `tests/seam_check.py`.
- [x] **Song transitions (trace build 96d569c, logs skate3_004/005, script
      `tests/trace_report.py`):**
  - Gap between songs: 1-70 ms between last frame and next song's first frame (decoder
    timing) - effectively none.
  - Picking a song in the game's music player changes song immediately (left song 1 at
    22 s of 109 s) -> mid-slot change = user action, easy to detect.
  - No usable "previous": order was 42 -> 1 -> 29 (shuffle-like). Our playlist must own
    previous/next.
  - Pause menu: music keeps decoding, no song change.
  - Original song (music off, song 17, 176 s) played all 30 segments to the end.
  - With custom audio, slot 42 (195 s) holding a 160 s track (silence after 160 s) was left
    at ~176 s, i.e. after ~16 s of silence, with no early exit in ipod.mpf. **Inference:
    the game skips a song after a stretch of silence.** Consequences: never feed long
    silence (radio mode always has audio); our Pause must not output silence for >10 s
    (or must expect the game to change song - then just re-sync). Verify once in Phase 1.
  - Every ~6 s segment is decoded on a fresh XMA context; resets arrive late, in batches.
    The engine already handles this.
- [x] **Hotkeys:** plain binds fire regardless of modifiers and the first matching bind wins,
      so Ctrl+F7 / Ctrl+F8 collide with F7 (menu) and F8 (fingerprint log). Pick other keys
      or register the chord binds before the plain ones.
- **Phase 1 decision pending:** the in-game Rebuild cannot run vgmstream. Options: decode
      originals once with the Python tool + vgmstream (simple), or decode in-process with the
      runtime's FFmpeg XMA decoder (check what rexruntime.dll exports).

### Phase 1 - our playlist drives the music (code done 2026-10-12, needs in-game test)
- [x] In-game decode of the game's songs: SDK `rex::audio::XmaStreamDecoder` (SDK 5df62be)
      + `skate3_custom_music_mus` (MUS reader, index builder, segment join). No FLAC copy
      of the originals is stored; they are decoded from Ipod_Stream.mus while playing
      (0.4-0.9 s per song on the worker thread). Join: audio starts at lead+192 (=576),
      missing tails (<=180 samples) predicted, 32-sample crossfade per join.
- [x] Index built in-game (byte-identical to Python); no prepare script needed.
- [x] Carrier layout: all 46 slots >= 19.9 min (budget 8000 nodes, 233 KB ipod.mpf),
      written at startup if missing/different. Validated (Python validator, vgmstream).
      **Not yet confirmed in game** (larger ipod.mpf).
- [x] Player (`skate3_custom_music_player`): slot position = clock; anchors per slot;
      slot ran out (<= 20 s left) -> continue; left early / restarted -> next entry;
      late prefetch segment of the previous slot (< 3 s) ignored by the engine.
- [x] F7: Previous / Next / Shuffle / Use my music / Pop-ups / Volume, playlist table
      (on/off, Play, up/down, bulk on/off). Saved to playlist.tsv. Toast shows the entry.
- [x] Rebuild writes tracks.tsv (your tracks); per-slot assignment and length fitting are
      legacy (`fit_layout`), manifest.tsv optional.
- [ ] In-game test: build 373ae46 (+SDK 5df62be). Check: decoder self-test line, "long
      iPod slots active", music plays from the playlist, Next/Prev, in-game song pick ->
      next entry, toast titles.
- [x] Your tracks convert in-game (user chose: extend the runtime's FFmpeg). SDK ac48976:
      libavformat + libswresample + aac/opus/vorbis/flac/alac/pcm/mp2/wmav1 decoders and
      matroska/mov/ogg/flac/wav/aiff/aac/w64 demuxers via an overlay config (FFmpeg
      submodule untouched). `rex::audio::DecodeAudioFile` also returns tags + cover;
      sidecars `<pcm>.tags` / `<pcm>.cover`, tracks.tsv has artist/album columns.
      Installed ffmpeg = fallback only.
- [ ] Pause: not offered (game appears to skip a song after ~16 s of silence).
- [ ] Hotkeys for previous/next: Ctrl+F7/F8 collide; pick others.

### Phase 2 - metadata
- [ ] ffprobe tags (title / artist / album); ffmpeg extracts embedded cover -> 256x256 RGBA.
- [ ] MusicBrainz search (cleaned "Artist - Title", strip yt-dlp id and "official video"
      noise), Cover Art Archive thumbnail; 1 req/s, User-Agent
      `dekp23-skate3-custom-music ( github.com/dekp23 )`; opt-in toggle; cached.
- [ ] AcoustID: user's API key entered in F7, `fpcalc` downloaded on first use; also
      identifies the 46 originals (titles currently unknown).
- [ ] Manual edit fields in F7.

### Phase 3 - covers in UI
- [ ] Pop-up: cover + title + artist + album, dekp23 brand kept. Playlist rows with thumbnails.

### Phase 4 - research (no promises)
- [ ] Decode EA "EB" BIG archives (`db.big`, `fedata.big`; music player UI is in
      `fedata.big`: `ID_MUSIC_PLAYER`, `Trackname`, `Artist`).
- [ ] Find the song table and the music player's title text; decide whether we can show
      the correct title natively or add songs to the game's own list.

## Needed from you
- Free AcoustID API key (acoustid.org) - Phase 2, not needed for 0-1.
- Pick hotkeys for previous / next (Ctrl+F7/F8 collide with existing binds).
- Optional: download with `yt-dlp --embed-metadata --embed-thumbnail -f bestaudio ...`
  so files carry tags and covers.

## Key facts (for whoever picks this up)
- iPod: 46 songs x 30 equal segments, EA-XMA 44.1 kHz stereo, Pathfinder v5.3 big-endian.
  `ipod.mpf` section 1 = one chain per song (entry wave 0 -> 30 segment nodes, wave =
  sample+1 -> end wave -1); section 2 = short versions (last 11 segments); parts numbered
  from 92 = 2 x 46 (suggests the game assumes 46 songs - inference).
- Each segment: 384-sample decoder delay at start, 515-713 samples overrun at end; the
  runtime maps decoded sample d of a segment to `timeline + d - 384`.
- Fingerprint: FNV-1a 64 over packet bytes [4, 260); 93,588 unique iPod packets,
  0 collisions with Game_Stream.
- The game reads music via `\Device\Harddisk0\Partition1\data\audio\music`
  (`game:` and `d:` link there); our overlay mounts `skate3music:` over it.
- `settings.toml` is rewritten by the settings menu (own keys only) - never store our
  settings there; ours live in `<library>\player.ini`.
- The `XmaContext ... exceeds buffer size 16384` log errors are pre-existing in the
  official v2.0.2 build (379 in one session), not ours.

## Tests (all passing at `9e4cf7e`)
```
# Python
py -m unittest discover -s tools/skate3_music_import/tests
# C++ (needs prepared library + sim streams; see test file headers)
test_custom_music  CUSTOM_DIR SIM.bin [CHAINS.txt]   # sample-exact replay, incl. 69-segment chain
test_library       ...                               # assign vs Python (500 cases), rebuild, reload under load, flexible lengths
test_mpf           IPOD.mpf INDEX.s3fp OUT.mpf       # byte-identical identity rewrite, modified graph checks
```
