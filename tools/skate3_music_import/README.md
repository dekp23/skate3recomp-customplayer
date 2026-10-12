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

1. Build this fork (see the repo README, or `windows/run_build.cmd`, which does it all).
2. Start the game. The first start builds the music index from the game's own files
   (about a second) and the long-slot layout; nothing to run by hand.
3. In game, press **F7** (dekp23 Custom Music):
   - **Playlist**: the 46 Skate 3 songs and your tracks in one list. Tick what should
     play, reorder with the arrows, **Play** any entry, **Shuffle**, **Previous / Next**.
     Saved in `<library>\playlist.tsv`.
   - **Add tracks...** / **Import Music folder** copy audio into `<library>\tracks` and
     convert it in the background (needs ffmpeg for now: `winget install Gyan.FFmpeg`);
     **Rescan tracks** re-reads that folder.
   - **Use my music** (off = the game's normal soundtrack), **Pop-ups**, **Volume**
     (`<library>\player.ini`).
4. Picking a song in the game's own music player skips to the next playlist entry.
5. A **NOW PLAYING** pop-up shows each new entry (bottom left).

Library folder: `custom_music` next to `skate3.exe` (cvar `skate3_custom_music_dir`
overrides; settings.toml is not used for it because the settings menu rewrites that file).

### How it works (no game file is modified)

- The game keeps playing its iPod "slots". Every decoded XMA frame of a slot is replaced
  with the playlist audio at the matching position, so the slot position is the clock.
- The game's own songs are decoded in-game from `Ipod_Stream.mus` with the runtime's XMA
  decoder (`rex::audio::XmaStreamDecoder`); no copy is stored.
- Every slot is lengthened to ~20 minutes by a rewritten `ipod.mpf`
  (`<library>\ipod_layout.mpf`, served through the recomp's virtual file system from
  `<library>\game_music`, hard links + the layout). When a slot does run out, the
  playlist simply continues. Requires the library and the game on the same drive.

### Limitations

- The in-game music player still shows the original titles/artists; the pop-up shows
  yours. Replacing the game's own text needs the title strings, which are in EA's
  compressed archives (not yet decoded).
- Only the default XMA decoder path is hooked (`xma_use_old_decoder = true`).
- The 46 game songs are listed as "Skate 3 song N" until metadata lookup (Phase 2).
- No pause: the game appears to skip a song after ~16 s of silence.

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

- iPod: 1380 segments = 46 songs x 30 equal-length segments, **confirmed by the MPF
  node graph**: section 1 holds one chain per song (entry node -> 30 segment nodes, wave =
  sample index + 1 -> end node); section 2 holds a short version of each song (last 11
  segments). Named variables: `ipodsection`, `ipod`, `replay`. Lengths 109-369 s.
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

1. *(done)* Inspector/validator; runtime replacement engine + hook; confirmed in game.
2. *(done)* Now-playing pop-up, F7 menu, in-game library rebuild.
3. *(done, needs in-game confirmation)* Song lengths fitted via the MPF node graph.
4. Replace titles in the game's own music player (decode EA "EB" archives or hook the
   text lookup).

Legal: use only your own game copy; don't distribute game files or copyrighted music.
