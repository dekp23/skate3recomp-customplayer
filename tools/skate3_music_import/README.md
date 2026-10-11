# skate3_music_import

Read-only tooling for Skate 3's (Xbox 360) interactive-music files, as installed by
skate3recomp under `game\data\audio\music\`. Milestone 1 of a custom-music import
project. **Nothing here modifies game files.**

Requires Python 3.8+. No third-party packages. `vgmstream-cli` is optional.

## Commands

Run from this folder (Windows: `py` instead of `python3`).

```
# 1. Back up first. Copies the folder and writes a SHA-256 manifest, verified after copy.
python3 skate3_music_import.py backup "C:\...\Skate3Recomp-Windows\game\data\audio\music"

# 2. Structure + per-stream codec/rate/channels/duration
python3 skate3_music_import.py inspect "...\music\ipod.mpf"            # first 40 rows
python3 skate3_music_import.py inspect "...\music\ipod.mpf" --limit 0  # all rows
python3 skate3_music_import.py inspect "...\music\ipod.mpf" --json > ipod.json

# 3. Consistency checks (exit 0 = PASS, 1 = errors, 2 = unparseable)
python3 skate3_music_import.py validate "...\music\ipod.mpf"

# 4. Optional: ask vgmstream for its reading of subsongs 1 and 2
python3 skate3_music_import.py probe "...\music\ipod.mpf" --subsong 1 --subsong 2 --vgmstream C:\path\vgmstream-cli.exe

# 5. Later: confirm the folder is still identical to a backup
python3 skate3_music_import.py verify "...\music" "...\music_backup_YYYYMMDD-HHMMSS"
```

The `.mus` is found automatically for `game.mpf`, `ipod.mpf`, `world.mpf` (pairs taken
from vgmstream's table, labelled there "Skate 1/2/3"). Use `--mus` otherwise.

Tests: `python3 -m unittest discover -s tests -v`

## Format facts and their sources

| Fact | Source | Status |
|---|---|---|
| `game/ipod/world.mpf` pair with `Game/Ipod/World_Stream.mus` in Skate 1/2/3 | vgmstream `src/meta/ea_eaac_mpf_mus.c` | documented upstream |
| MPF magic `PFDx`, Pathfinder v5, minor 2 or 3; either byte order | same file | documented upstream |
| MUS audio is EAAC: SNR header + SNS blocks | same file, `ea_eaac.c` | documented upstream |
| MUS v5.2 table at 0x08, 0x0C-byte entries; v5.3 table at 0x28, 0x1C-byte entries, offsets ×0x10/×0x80 | same file | documented upstream |
| MPF track info carries a MUS checksum = first 4 bytes of MUS | vgmstream enforces it | **whether the game checks it: unknown** |
| Skate 3 X360 files are big-endian, which minor version, which codec | — | **unknown until run on the real files** |

## Why not MPFmaster directly

MPFmaster (xan1242) reads structs in native little-endian with no byte-swapping; its
source and README list big-endian/console support as TODO. If the X360 files are
big-endian (likely, unverified), MPFmaster will misread them. Its sample-update path
also targets the EA-SCHL/GSTR and EALayer3 (v5.3-style) layouts. It remains the best
reference for the write side.

## Verification done

- 10 unit tests over synthetic fixtures (both endians, v5.2 and v5.3, corruption cases).
- Synthetic PCM16BE pairs in all four layout/endian combinations were read by an actual
  `vgmstream-cli` build (upstream master) with matching rate, channels and sample counts.
- **Not yet run on real Skate 3 files.** The fixtures prove agreement with vgmstream's
  documented layout, not with the game.

## Roadmap

1. *(this)* Inspect/validate/backup. Run on the real `ipod.mpf` and report output.
2. Identify the codec. If EA-XMA (code 0x03, X360 typical), an encoder is the main
   obstacle; if EALayer3, EA's `sx`/community encoders may apply. Decide from data.
3. Byte-exact round-trip: rebuild the MUS from its own parsed entries and confirm
   identical bytes. Proves the writer before any audio changes.
4. Replace one stream with an encoded user track of equal or shorter length, update MPF
   duration/offset entries, test in-game.
5. Folder-to-playlist converter (option 2 of the handoff), or runtime hook (option 3).

Legal: use only your own game copy; don't distribute game files or copyrighted music.
