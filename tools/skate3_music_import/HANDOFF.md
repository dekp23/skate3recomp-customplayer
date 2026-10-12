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
| Recomp fork | `dekp23/skate3recomp-customplayer`, branch `custom-music-import` (last: `9e4cf7e`) |
| SDK fork | `dekp23/rexglue-skate3-customplayer`, branch `custom-music-hook` (`2832e85`: frame filter hook + public imgui pin) |
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

### Phase 0 - spikes (verify before building)
- [ ] Logging build: measure the gap between game songs; confirm an in-game skip is
      detectable (new song mid-slot); check whether the iPod has "previous".
- [ ] Can rexglue's ImGui backend show our own textures (imgui 1.92
      `ImGuiBackendFlags_RendererHasTextures` / `ImTextureData`)? If not, scope the SDK change.
- [ ] Decode all 1380 iPod segments with vgmstream (+FFmpeg), join 30 per song, check
      seams are click-free and lengths equal the SNR sample counts.

### Phase 1 - our playlist drives the music
- [ ] Library: originals decoded once to FLAC; your tracks converted to FLAC.
- [ ] Engine: dr_flac decoding; "radio" scheduling - every iPod frame plays our playlist.
      Position = f(game slot position), so prefetching contexts stay exact.
- [ ] All 46 slots lengthened to the maximum (~15-20 min, existing MPF rewriter, node
      budget 8000) so the game rarely changes song by itself.
- [ ] Track change rules: our track ends -> next; game song changes mid-slot (skip) -> next.
- [ ] F7 playlist editor: checkbox per entry (originals + yours), reorder, shuffle,
      repeat; saved in the library. Prev / Next / Pause buttons; hotkeys Ctrl+F7 / Ctrl+F8.
- [ ] Remove the per-slot assignment + length-fit table from the menu.

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
- Confirm hotkeys Ctrl+F7 (previous) / Ctrl+F8 (next), or pick others.
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
