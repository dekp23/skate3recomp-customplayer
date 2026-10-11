# skate3_music_import

Custom music for Skate 3 in skate3recomp. Your own tracks replace the in-game iPod
songs **at the recomp's XMA decoder output**. Game files are never modified.

Two parts:

- **This Python tool** (Python 3.8+, no packages; `ffmpeg` for `prepare`): inspects the
  game's music files, fingerprints every iPod XMA packet, converts your music and
  assigns it to the 46 in-game song slots.
- **Runtime** (`src/skate3_custom_music*.cpp` + a frame-filter hook in the rexglue SDK
  fork): for each decoded XMA frame whose packet is an iPod packet, writes your PCM at
  the exact song position instead. SFX, menu and world music pass through.

## Using it

1. Build this fork of skate3recomp (see the repo README, "Building from Source"). The
   submodule `third_party/rexglue-sdk` points at the fork with the hook
   (`dekp23/rexglue-skate3-customplayer`, branch `custom-music-hook`):
   `git submodule sync --recursive && git submodule update --init --recursive`.
2. Install ffmpeg (Windows: `winget install Gyan.FFmpeg`), then from this folder:
   ```
   py skate3_music_import.py backup  "C:\...\game\data\audio\music"
   py skate3_music_import.py prepare "C:\...\game\data\audio\music" "C:\path\to\my music"
   ```
   This writes `...\music\custom\` (index, manifest, converted PCM; ~10 MB per minute)
   and prints which track went to which slot. Re-run any time your music changes.
3. Start the game. The log should show
   `custom music: loaded 93588 fingerprints, N/46 iPod songs replaced`, then on first
   iPod playback `custom music: first iPod frame replaced`. Shutdown logs statistics.

Options: `--fill original` keeps the game's song for unused slots (default `repeat`
reuses your tracks); `--gain-db -3` adjusts level. Settings: `skate3_custom_music`
(on/off), `skate3_custom_music_dir` (folder override).

### Current limitations (honest list)

- **Song length is fixed by the game slot.** A track plays for exactly as long as the
  in-game song it replaces (109–369 s). Shorter tracks end in silence until the slot
  ends; longer ones are cut. `prepare` best-fits lengths and reports both cases.
- Track titles shown in-game are still the original artists/titles.
- Only the default XMA decoder path is hooked (`xma_use_old_decoder = true`, the default).
- **Not yet confirmed in-game.** Everything below "Verification" is offline. The key
  assumption is that the game plays iPod music through hardware XMA contexts; the log
  lines above confirm or refute it on first run.

## Inspection commands

```
py skate3_music_import.py inspect  "...\music\ipod.mpf" [--limit 0] [--json]
py skate3_music_import.py validate "...\music\ipod.mpf"          # exit 0 PASS / 1 errors
py skate3_music_import.py index    "...\music\ipod.mpf" --out ipod.s3fp   # lists songs
py skate3_music_import.py probe    "...\music\ipod.mpf" --subsong 1 --vgmstream PATH
py skate3_music_import.py verify   "...\music" "...\music_backup_YYYYMMDD-HHMMSS"
```

## Why runtime replacement, not re-encoding the game files

- The music is EA-XMA (XMA2 in EA's SNR/SNS wrapper). The only XMA2 encoder is
  Microsoft's, licensed to registered Xbox developers. FFmpeg's `xma1`, `xma2` and
  `wmapro` are decode-only (checked with `ffmpeg -codecs`).
- MPFmaster reads little-endian only (its own TODO); these files are big-endian.
- rexglue already decodes every XMA frame with FFmpeg in `XmaContext::DecodeOldFrame`,
  so replacing decoded PCM there needs no encoder and no game-file changes.
- rexglue's bundled FFmpeg is trimmed to `libavcodec`/`libavutil` with XMA/WMA Pro only,
  so user audio is converted offline by `prepare` to raw 44.1 kHz stereo PCM.

## Format facts

Upstream-documented (vgmstream `src/meta/ea_eaac_mpf_mus.c`, `ea_eaac.c`,
`ea_eaac_streamfile.h`): `game/ipod/world.mpf` pair with `*_Stream.mus` in Skate 1/2/3;
MPF magic `PFDx`, Pathfinder v5.2/5.3, either byte order; EAAC SNR+SNS audio; MUS v5.3
table at 0x28 with 0x1C-byte entries; EA-XMA SNS blocks = header, samples, per-stream
`size*4` + XMA packet data.

Measured on the real v2.0.2 install (2026-10-11):

| File | Pathfinder | Endian | Tracks | Samples | Codec | Validate |
|---|---|---|---|---|---|---|
| `ipod.mpf` + `Ipod_Stream.mus` (180,580,480 B) | v5.3 | big | 2 (track 1 empty, checksum `F1F1F1F1`) | 1380 | EA-XMA 44.1 kHz stereo | PASS |
| `game.mpf` + `Game_Stream.mus` (28,052,480 B) | v5.3 | big | 1 | 1074 | EA-XMA 44.1 kHz stereo | PASS |
| `world.mpf` | v5.3 | big | 1 | 5725 | MUS not checked | — |

- iPod: 1380 segments = **46 runs of 30 equal-length segments**. Inference (not yet
  cross-checked against the MPF node graph): 46 songs, each cut into 30 parts;
  lengths 109–369 s, total 145.2 min.
- One trimmed XMA packet per SNS block (< 2048 bytes; the game pads it for hardware).
  93,589 packets; block samples sum exactly to each SNR sample count.
- Every segment: first packet decodes 384 samples more than it declares (decoder
  delay); middle packets exact; 515–713 samples of overrun at the end. The runtime
  maps decoded sample `d` of a segment to song position `segment_start + d - 384`.
- Fingerprint = FNV-1a 64 of packet bytes [4, 260): 93,588 unique (1 duplicate,
  117 packets too short and bridged at runtime); 0 collisions with `Game_Stream.mus`.

## Verification

- 17 Python unit tests (`py -m unittest discover -s tests`).
- vgmstream + FFmpeg decodes real iPod segment 1 to 9.50 s of non-silent stereo audio.
- C++ engine replay on real data (`tests/cpp/test_custom_music.cpp` + `tests/sim_stream.py`):
  real iPod packets padded to 2048 bytes, two songs interleaved on two contexts plus
  `Game_Stream` packets, an unassigned song and a reset-and-reused context.
  Result: 22,560/22,560 frames replaced, **0 wrong samples** versus the expected user
  PCM, 0 false replacements. Clean under ASan/UBSan, `-Wall -Wextra -Werror`, C++17/20/23.
- rexglue `xma_context.cpp` with the hook and the glue file pass Clang 18 C++23
  semantic checks against the real SDK headers. **The full recomp has not been built
  here** (needs your game dump; too large for this machine).

```
c++ -std=c++23 -O2 -Isrc src/skate3_custom_music.cpp tools/skate3_music_import/tests/cpp/test_custom_music.cpp -o test_custom_music
python3 tools/skate3_music_import/tests/sim_stream.py MUSIC/ipod.mpf MUSIC/game.mpf sim.bin
./test_custom_music MUSIC/custom sim.bin
```

## Roadmap

1. *(done)* Inspector/validator; codec identified; runtime replacement engine + hook.
2. **Next: first in-game test** on Windows; read the log lines above.
3. Decode the MPF node graph: confirm song boundaries, find title strings, map in-game
   "next track" behaviour.
4. Remove the slot-length limit: build longer "carrier" songs by reusing existing XMA
   packets in a rewritten `.mus`/`.mpf` (no encoder needed, since the runtime replaces
   the audio anyway).

Legal: use only your own game copy; don't distribute game files or copyrighted music.
