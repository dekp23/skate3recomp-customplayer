# Custom Music (dekp23): setup guide

Play your own music on the Skate 3 iPod, mixed with the game's 46 songs in one playlist.
You need your own legally obtained copy of Skate 3 (ISO), as for Skate3Recomp itself.
Nothing here modifies your game files.

There are two ways to get it. **Option A is the one-time setup for players.** Option B is
for building it yourself from source.

---

## Option A: download the release (recommended)

1. Download **Skate3Recomp-CustomMusic-Windows.zip** from the
   [releases page](https://github.com/dekp23/skate3recomp-customplayer/releases).
2. Extract it to a folder you control (for example `C:\Games\Skate3Recomp-CustomMusic`).
   Keep everything in that one folder: the game files, your saves and your music all live
   next to `skate3.exe`, so the folder can be moved or backed up as a whole.
3. Run `skate3.exe`, click **Select ISO**, choose your Skate 3 ISO and wait for the files to
   be extracted. Then **Start Game**.
   (Already have Skate3Recomp installed? You can instead copy `skate3.exe` and the `.dll`
   files from the zip into that folder; keep a backup of the originals.)
4. In game, press **F7** to open the music menu. That's it.

The first start with custom music takes a few extra seconds: the game builds its music
index and the "long song slots" file from your game files (once).

## Option B: build it yourself (Windows)

Needs Windows 10/11 with **winget** ("App Installer" from the Microsoft Store), an
internet connection, about **25 GB free on C:** and 20-40 minutes the first time.

1. Install the official Skate3Recomp first (see the main README) and set it up with your
   ISO once.
2. Copy the folder `tools/skate3_music_import/windows` from this repository into your
   Skate3Recomp folder and rename it to `custom_music_build`, so you have
   `...\Skate3Recomp-Windows\custom_music_build\run_build.cmd`.
3. Double-click `run_build.cmd` and keep the window open. Approve the Windows prompts when
   build tools are installed. It:
   - installs missing tools with winget (Git, CMake, Ninja, LLVM, Python, Visual Studio
     2022 Build Tools),
   - downloads this repository to `C:\s3build\skate3recomp` and the game's title update
     from xboxunity.net,
   - builds the game (`status.txt` shows the current step, `build.log` everything),
   - creates `C:\s3build\Skate3Recomp-CustomMusic`, which uses your existing game folder
     (linked, not copied) and **copies** your saves and settings once; your original
     install is only read,
   - also writes `C:\s3build\Skate3Recomp-CustomMusic-Windows.zip`, the release package
     for Option A.
4. Run `C:\s3build\Skate3Recomp-CustomMusic\skate3.exe` and press **F7**.

To update later, run `run_build.cmd` again (close the game first). Rebuilds take ~10-15
minutes. Your music, playlist and settings are kept.

---

## Using it

Press **F7** in game.

| Control | What it does |
|---|---|
| **Add tracks...** | Pick files; they are copied into your music folder and converted in the background. |
| **Import Music folder** | Copies everything from Windows' Music folder (including subfolders). |
| **Rescan tracks** | Re-reads the `tracks` folder (after you copy files there yourself). |
| **Open tracks folder** | Opens the folder in Explorer. |
| **Previous / Next**, **Play** | Move through the playlist. Picking a song in the game's own music player also skips to the next entry. |
| **Shuffle** | Random order (the current song keeps playing). |
| Playlist checkboxes, arrows | Choose which songs play (game songs and yours), and their order. **Game songs off** / **My tracks on** etc. switch groups at once. |
| **Use my music** | Off = the game's normal soundtrack. |
| **Pop-ups** | "Now playing" pop-up when a new song starts. |
| **Volume** | Volume of the playlist (0-200 %). |
| **Look up song info online (MusicBrainz)** | Off by default. When on, finds the real title, artist, album and cover for your tracks. See *Privacy*. |

**Formats:** mp3, m4a (AAC/ALAC), webm/mkv/opus (e.g. from yt-dlp), ogg, flac, wav, aiff,
wma, and the audio track of mp4 videos. Conversion happens inside the game; nothing to
install. Tip for yt-dlp users: `yt-dlp -x --embed-metadata --embed-thumbnail URL` stores
title, artist and cover in the file, which the game reads directly.

**Where things are:** everything is in the `custom_music` folder next to `skate3.exe`:
your files in `custom_music\tracks`, converted copies (`.pcm`), the playlist and settings.
Deleting a file from `tracks` and clicking **Rescan tracks** removes it.

## Privacy

Online lookups are **off** until you tick the box. When on, for each of your tracks the game
sends its title (from the file's tags or its file name) to `musicbrainz.org`, one request
per second, and downloads cover images from `coverartarchive.org`. Results are saved in
`custom_music\metadata.tsv` and `custom_music\covers`, so each track is looked up once.
Nothing else is sent.

## Troubleshooting

| Problem | What to do |
|---|---|
| F7 says "Not active" | The game files could not be read. Check that the game was installed (Select ISO) and see `logs\skate3_*.log` for lines starting with `custom music:`. |
| A track does not convert | The status line names the file and the reason. Re-download it in another format (mp3/m4a/opus). |
| Wrong or missing song info | Click **Look up now** to retry the ones not found. Tracks whose files carry tags (title + artist) use the tags. |
| The game changes song by itself every few minutes | The long-slot file could not be set up, usually because `custom_music` is on a different drive than the game folder. Keep both in the same folder (Option A) or on the same drive. The log says `iPod slots keep their original length`. |
| You want a clean start | Close the game and delete the `custom_music` folder (your `tracks` are inside it: move them out first). It is rebuilt on the next start. |
| Something else | Turn on the diagnostic log: start `skate3.exe --skate3_custom_music_trace` (or add the line `skate3_custom_music_trace = true` on its own line in `settings.toml`), play, and share `logs\skate3_*.log`. |

## Uninstall

Delete the Skate3Recomp-CustomMusic folder (Option A: the whole folder you extracted;
Option B: `C:\s3build`). Your original Skate3Recomp install is not touched by either option.
