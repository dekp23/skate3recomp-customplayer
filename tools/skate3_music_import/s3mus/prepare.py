"""Prepare a custom-music folder for the skate3recomp runtime override.

Output folder layout (read by src/skate3_custom_music.cpp):
    ipod.s3fp            fingerprint index (see xma.write_index)
    manifest.tsv         one line per in-game song slot that gets user audio
    NNN.pcm              raw 44.1 kHz stereo s16le PCM per user track

manifest.tsv format (UTF-8, '#' comments ignored):
    s3music<TAB>1
    <song_index><TAB><pcm_file><TAB><num_samples><TAB><source title>
"""
from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Sequence

from .xma import Song

# Any container FFmpeg can read; only the first audio stream is used (video is ignored).
AUDIO_EXTS = {".mp3", ".flac", ".ogg", ".oga", ".opus", ".m4a", ".aac", ".wav", ".wma", ".aiff",
              ".aif", ".alac", ".ape", ".wv", ".webm", ".mka", ".mp4", ".m4v", ".mkv", ".mov"}
RATE = 44100
BYTES_PER_SAMPLE_FRAME = 4  # stereo s16
MANIFEST = "manifest.tsv"
INDEX_NAME = "ipod.s3fp"


@dataclass
class Track:
    source: Path
    pcm_name: str
    num_samples: int

    @property
    def seconds(self) -> float:
        return self.num_samples / RATE


@dataclass
class Assignment:
    song: int
    track: Optional[Track]   # None => original game audio kept


def find_tracks(folder: Path) -> List[Path]:
    files = [p for p in sorted(folder.rglob("*")) if p.is_file() and p.suffix.lower() in AUDIO_EXTS]
    return files


def convert(ffmpeg: str, src: Path, dst: Path, gain_db: float = 0.0) -> int:
    cmd = [ffmpeg, "-nostdin", "-v", "error", "-y", "-i", str(src), "-vn", "-map", "0:a:0",
           "-ac", "2", "-ar", str(RATE)]
    if gain_db:
        cmd += ["-af", f"volume={gain_db}dB"]
    cmd += ["-f", "s16le", "-acodec", "pcm_s16le", str(dst)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg failed on {src.name}: {proc.stderr.strip()[:300]}")
    return dst.stat().st_size // BYTES_PER_SAMPLE_FRAME


def assign(songs: Sequence[Song], tracks: Sequence[Track], fill: str = "repeat") -> List[Assignment]:
    """Best-fit: longest tracks first, each into the shortest free slot that holds it whole;
    if none holds it, the longest free slot (track gets truncated).
    fill='repeat' reuses tracks for leftover slots (same rule); 'original' leaves them alone."""
    free = {s.index: s.num_samples for s in songs}
    out = {}

    def place(track: Track):
        fits = [(n, i) for i, n in free.items() if n >= track.num_samples]
        slot = min(fits)[1] if fits else max((n, i) for i, n in free.items())[1]
        out[slot] = track
        del free[slot]

    ordered = sorted(tracks, key=lambda t: -t.num_samples)
    for t in ordered:
        if not free:
            break
        place(t)
    if fill == "repeat" and ordered:
        while free:
            # For each leftover slot (longest first), pick the track whose length is
            # closest to it without exceeding, else the longest track.
            slot_len, slot = max((n, i) for i, n in free.items())
            fits = [t for t in ordered if t.num_samples <= slot_len]
            out[slot] = max(fits, key=lambda t: t.num_samples) if fits else ordered[0]
            del free[slot]
    return [Assignment(s.index, out.get(s.index)) for s in songs]


def write_manifest(out_dir: Path, assignments: Sequence[Assignment]) -> None:
    lines = ["# skate3recomp custom music manifest", "s3music\t1"]
    for a in assignments:
        if a.track is None:
            continue
        title = a.track.source.stem.replace("\t", " ")
        lines.append(f"{a.song}\t{a.track.pcm_name}\t{a.track.num_samples}\t{title}")
    (out_dir / MANIFEST).write_text("\n".join(lines) + "\n", encoding="utf-8")


def report(songs: Sequence[Song], assignments: Sequence[Assignment]) -> List[str]:
    rows = []
    for a, s in zip(assignments, songs):
        if a.track is None:
            rows.append(f"  slot {s.index:>2} ({s.seconds:6.1f}s)  <- original game song")
            continue
        diff = a.track.seconds - s.seconds
        note = (f"truncated by {diff:.1f}s" if diff > 0.05
                else f"{-diff:.1f}s silence at end" if diff < -0.05 else "exact")
        rows.append(f"  slot {s.index:>2} ({s.seconds:6.1f}s)  <- {a.track.source.name} "
                    f"({a.track.seconds:.1f}s, {note})")
    return rows


def prepare(songs: Sequence[Song], index_src: Path, music_in: Path, out_dir: Path,
            ffmpeg: Optional[str] = None, fill: str = "repeat", gain_db: float = 0.0,
            log=print) -> List[Assignment]:
    ffmpeg = ffmpeg or shutil.which("ffmpeg") or shutil.which("ffmpeg.exe")
    if not ffmpeg:
        raise RuntimeError("ffmpeg not found on PATH (Windows: winget install Gyan.FFmpeg); "
                           "or pass --ffmpeg")
    sources = find_tracks(music_in)
    if not sources:
        raise RuntimeError(f"no audio files found in {music_in}")
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("*.pcm"):
        old.unlink()
    tracks: List[Track] = []
    for i, src in enumerate(sources):
        name = f"{i:03d}.pcm"
        log(f"converting {src.name}")
        n = convert(ffmpeg, src, out_dir / name, gain_db)
        if n == 0:
            log(f"  skipped: no audio decoded")
            continue
        tracks.append(Track(src, name, n))
    if len(tracks) > len(songs):
        log(f"note: {len(tracks)} tracks but only {len(songs)} in-game song slots; "
            f"{len(tracks) - len(songs)} shortest-fit leftovers are unused")
    assignments = assign(songs, tracks, fill)
    used = {a.track.pcm_name for a in assignments if a.track}
    for t in tracks:
        if t.pcm_name not in used:
            (out_dir / t.pcm_name).unlink()
    shutil.copyfile(index_src, out_dir / INDEX_NAME)
    write_manifest(out_dir, assignments)
    for row in report(songs, assignments):
        log(row)
    return assignments
