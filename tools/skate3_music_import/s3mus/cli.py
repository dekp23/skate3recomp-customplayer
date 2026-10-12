"""skate3_music_import: read-only inspection, validation, probing and backup
for Skate 3 (Xbox 360) Pathfinder .mpf + EAAC .mus music files.

Nothing in this module modifies game files. `backup` only copies them.
"""
from __future__ import annotations

import argparse
import dataclasses
import datetime as _dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional

from .mpf import MpfError, MpfFile, parse_mpf
from .mus import MusError, MusFile, parse_mus

# Pairs known from vgmstream's open_mapfile_pair table ("Skate 1/2/3").
KNOWN_PAIRS = {
    "game.mpf": "Game_Stream.mus",
    "ipod.mpf": "Ipod_Stream.mus",
    "world.mpf": "World_Stream.mus",
}
DURATION_TOLERANCE_MS = 2


# ---------------------------------------------------------------- helpers

def _find_ci(directory: Path, name: str) -> Optional[Path]:
    """Case-insensitive lookup (Windows is case-insensitive; Linux test boxes are not)."""
    exact = directory / name
    if exact.exists():
        return exact
    low = name.lower()
    for p in directory.iterdir():
        if p.name.lower() == low:
            return p
    return None


def resolve_pair(mpf_path: Path, mus_override: Optional[Path]) -> Path:
    if mus_override:
        return mus_override
    pair = KNOWN_PAIRS.get(mpf_path.name.lower())
    candidates = [pair] if pair else []
    candidates.append(mpf_path.with_suffix(".mus").name)
    for c in candidates:
        found = _find_ci(mpf_path.parent, c)
        if found:
            return found
    raise SystemExit(f"error: no .mus found for {mpf_path.name}; pass --mus")


def load_pair(mpf_path: Path, mus_path: Path, walk: bool = True):
    mpf_bytes = mpf_path.read_bytes()
    mpf = parse_mpf(mpf_bytes)
    mus = parse_mus(mus_path.read_bytes(), mpf.minor, mpf.endian, walk_streams=walk)
    return mpf_bytes, mpf, mus


def _jsonable(obj):
    if dataclasses.is_dataclass(obj):
        return {k: _jsonable(v) for k, v in dataclasses.asdict(obj).items()}
    if isinstance(obj, (list, tuple)):
        return [_jsonable(v) for v in obj]
    if isinstance(obj, dict):
        return {k: _jsonable(v) for k, v in obj.items()}
    return obj


# ---------------------------------------------------------------- validate

@dataclasses.dataclass
class Finding:
    level: str   # "ERROR", "WARN", "INFO"
    message: str


def validate_pair(mpf: MpfFile, mus: MusFile, track_index: int = 0) -> List[Finding]:
    out: List[Finding] = []
    add = lambda lvl, msg: out.append(Finding(lvl, msg))

    if track_index >= len(mpf.tracks):
        add("ERROR", f"MPF has {len(mpf.tracks)} track(s); track {track_index} does not exist")
        return out
    track = mpf.tracks[track_index]
    rng = mpf.samples_for_track(track_index)

    add("INFO", f"MPF Pathfinder v{mpf.major}.{mpf.minor}, {mpf.endian_name}-endian, "
                f"{len(mpf.tracks)} track(s), {len(mpf.samples)} sample entries")
    add("INFO", f"MUS layout {mus.layout}, {len(mus.entries)} table entries, {mus.size} bytes")

    if mpf.minor not in (2, 3):
        add("ERROR", f"Pathfinder minor version {mpf.minor} is outside vgmstream's supported 5.2-5.3")
    if mpf.offsets["mapfilelen"] != mpf.size:
        add("WARN", f"MPF mapfilelen 0x{mpf.offsets['mapfilelen']:X} != file size 0x{mpf.size:X}")

    if len(rng) == 0:
        add("INFO", f"track {track_index} is empty (no samples; checksum field "
                    f"0x{track.mus_checksum:08X} is not checked); it does not use this MUS")
        return out

    if track.is_ram:
        add("WARN", f"track {track_index} is a RAM track ({track.num_subbanks} sub-banks); "
                    f"per-sample checks below assume STREAM layout")
    elif track.mus_checksum == 0:
        add("INFO", "MPF track checksum is 0 (vgmstream skips the check in that case)")
    elif track.mus_checksum != mus.checksum:
        add("ERROR", f"checksum mismatch: MPF track {track_index} expects 0x{track.mus_checksum:08X}, "
                     f"MUS starts with 0x{mus.checksum:08X}")
    else:
        add("INFO", f"checksum 0x{mus.checksum:08X} matches")

    if not track.is_ram and len(rng) != len(mus.entries):
        add("WARN", f"track {track_index} owns {len(rng)} MPF samples but MUS table has "
                    f"{len(mus.entries)} entries")

    codecs, rates, chans = set(), set(), set()
    for ent in mus.entries:
        tag = f"MUS entry {ent.index}"
        if ent.error:
            add("ERROR", f"{tag}: {ent.error}")
            continue
        s = ent.snr
        codecs.add(s.codec_name); rates.add(s.sample_rate); chans.add(s.channels)
        if ent.sns is not None:
            if not ent.sns.terminated:
                add("ERROR", f"{tag}: SNS walk failed ({ent.sns.error})")
            elif ent.sns_size is not None and ent.sns.size > ent.sns_size:
                add("WARN", f"{tag}: SNS blocks span {ent.sns.size} bytes, table says {ent.sns_size}")
        if not track.is_ram and ent.index < len(rng):
            sample = mpf.samples[rng.start + ent.index]
            snr_ms = round(s.duration_s * 1000)
            if abs(sample.duration_ms - snr_ms) > DURATION_TOLERANCE_MS:
                add("WARN", f"{tag}: MPF duration {sample.duration_ms} ms vs SNR {snr_ms} ms")
            if mus.layout == "v5.3" and sample.offset * 0x80 != ent.sns_offset:
                add("WARN", f"{tag}: MPF sample offset*0x80=0x{sample.offset * 0x80:X} "
                            f"!= MUS SNS offset 0x{ent.sns_offset:X}")

    if codecs:
        add("INFO", f"codecs: {', '.join(sorted(codecs))}; sample rates: "
                    f"{', '.join(map(str, sorted(rates)))}; channels: {', '.join(map(str, sorted(chans)))}")
    return out


# ---------------------------------------------------------------- commands

def cmd_inspect(args) -> int:
    mpf_path = Path(args.mpf)
    mus_path = resolve_pair(mpf_path, Path(args.mus) if args.mus else None)
    _, mpf, mus = load_pair(mpf_path, mus_path, walk=not args.no_walk)

    if args.json:
        print(json.dumps({"mpf_path": str(mpf_path), "mus_path": str(mus_path),
                          "mpf": _jsonable(mpf), "mus": _jsonable(mus)}, indent=2))
        return 0

    print(f"MPF  {mpf_path}  ({mpf.size} bytes)")
    print(f"     Pathfinder v{mpf.major}.{mpf.minor} rel {mpf.release}.{mpf.prerelease}, "
          f"{mpf.endian_name}-endian, project {mpf.project_id}")
    print(f"     tracks={mpf.num_tracks} sections={mpf.num_sections} events={mpf.num_events} "
          f"routers={mpf.num_routers} vars={mpf.num_named_vars} nodes={mpf.num_nodes} "
          f"samples={len(mpf.samples)}")
    for t in mpf.tracks:
        r = mpf.samples_for_track(t.index)
        print(f"     track {t.index}: samples [{r.start},{r.stop}) subbanks={t.num_subbanks} "
              f"checksum=0x{t.mus_checksum:08X} aram={t.max_aram} mram={t.max_mram}")
    print(f"MUS  {mus_path}  ({mus.size} bytes)")
    print(f"     layout {mus.layout}, checksum 0x{mus.checksum:08X}, entries={len(mus.entries)}"
          + (f" (declared {mus.declared_count})" if mus.declared_count is not None else ""))

    shown = mus.entries if args.limit <= 0 else mus.entries[: args.limit]
    print(f"\n  {'#':>4} {'idx':>5} {'sub':>4} {'SNR off':>10} {'SNS off':>10} "
          f"{'codec':<18} {'Hz':>6} {'ch':>2} {'type':<6} {'loop':>4} {'secs':>8} {'blocks':>6}")
    for e in shown:
        if e.error:
            print(f"  {e.index:>4} {e.id_index:>5} {e.sub_index:>4} 0x{e.snr_offset:08X} "
                  f"0x{e.sns_offset:08X} ERROR: {e.error}")
            continue
        s = e.snr
        blocks = e.sns.blocks if e.sns else "-"
        print(f"  {e.index:>4} {e.id_index:>5} {e.sub_index:>4} 0x{e.snr_offset:08X} "
              f"0x{e.sns_offset:08X} {s.codec_name:<18} {s.sample_rate:>6} {s.channels:>2} "
              f"{s.type_name:<6} {s.loop_flag:>4} {s.duration_s:>8.2f} {blocks:>6}")
    if len(shown) < len(mus.entries):
        print(f"  ... {len(mus.entries) - len(shown)} more (use --limit 0 for all)")
    return 0


def cmd_validate(args) -> int:
    mpf_path = Path(args.mpf)
    mus_path = resolve_pair(mpf_path, Path(args.mus) if args.mus else None)
    try:
        _, mpf, mus = load_pair(mpf_path, mus_path)
    except (MpfError, MusError) as exc:
        print(f"ERROR  {exc}")
        return 2
    findings = validate_pair(mpf, mus, args.track)
    for f in findings:
        print(f"{f.level:<5}  {f.message}")
    errors = sum(f.level == "ERROR" for f in findings)
    warns = sum(f.level == "WARN" for f in findings)
    print(f"\nresult: {'FAIL' if errors else 'PASS'} ({errors} error(s), {warns} warning(s))")
    return 1 if errors else 0


def cmd_probe(args) -> int:
    exe = args.vgmstream or shutil.which("vgmstream-cli") or shutil.which("vgmstream-cli.exe")
    if not exe:
        print("vgmstream-cli not found on PATH; pass --vgmstream PATH (optional dependency)")
        return 3
    mpf_path = Path(args.mpf)
    subsongs = args.subsong or [1]
    rc = 0
    for s in subsongs:
        cmd = [exe, "-m", "-s", str(s), str(mpf_path)]
        print("$ " + " ".join(cmd))
        proc = subprocess.run(cmd, capture_output=True, text=True)
        sys.stdout.write(proc.stdout)
        if proc.stderr:
            sys.stdout.write(proc.stderr)
        rc = rc or proc.returncode
    return rc


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_backup(args) -> int:
    src = Path(args.music_dir)
    if not src.is_dir():
        print(f"error: {src} is not a directory")
        return 2
    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    dst = Path(args.dest) if args.dest else src.parent / f"music_backup_{stamp}"
    if dst.exists():
        print(f"error: {dst} already exists; refusing to overwrite")
        return 2
    shutil.copytree(src, dst)
    manifest: Dict[str, str] = {}
    for p in sorted(src.rglob("*")):
        if p.is_file():
            rel = p.relative_to(src).as_posix()
            h_src, h_dst = _sha256(p), _sha256(dst / rel)
            if h_src != h_dst:
                print(f"error: copy verification failed for {rel}")
                return 1
            manifest[rel] = h_src
    (dst / "SHA256SUMS.json").write_text(json.dumps(manifest, indent=2))
    print(f"backed up {len(manifest)} file(s) to {dst} (SHA-256 verified)")
    return 0


def cmd_verify(args) -> int:
    """Compare a music folder against a backup's SHA256SUMS.json."""
    music = Path(args.music_dir)
    manifest = json.loads((Path(args.backup_dir) / "SHA256SUMS.json").read_text())
    changed = [rel for rel, h in manifest.items()
               if not (music / rel).exists() or _sha256(music / rel) != h]
    for rel in changed:
        print(f"CHANGED  {rel}")
    print("identical to backup" if not changed else f"{len(changed)} file(s) differ from backup")
    return 1 if changed else 0


def _build_ipod_index(mpf_path: Path, mus_override: Optional[Path], out: Path, quiet=False):
    from .xma import build_index, write_index
    mus_path = resolve_pair(mpf_path, mus_override)
    mus_bytes = mus_path.read_bytes()
    mpf = parse_mpf(mpf_path.read_bytes())
    mus = parse_mus(mus_bytes, mpf.minor, mpf.endian, walk_streams=False)
    entries, songs, by_song, st = build_index(mus_bytes, mus)
    write_index(out, entries, by_song)
    if not quiet:
        print(f"index: {st.segments} segments -> {st.songs} songs, {st.packets} packets, "
              f"{len(entries)} fingerprints ({st.dup_hashes} duplicate, {st.short_packets} too short), "
              f"block/SNR sample mismatches: {len(st.sample_mismatch)}")
    if st.sample_mismatch:
        raise MusError(f"block sample totals disagree with SNR headers: {st.sample_mismatch[:3]}")
    return songs


def cmd_index(args) -> int:
    songs = _build_ipod_index(Path(args.mpf), Path(args.mus) if args.mus else None, Path(args.out))
    for s in songs:
        print(f"  song {s.index:>2}: segments {s.first_segment:>4}-{s.first_segment + s.num_segments - 1:<4} "
              f"{s.seconds:7.1f}s")
    print(f"wrote {args.out}")
    return 0


def cmd_prepare(args) -> int:
    import tempfile
    from .prepare import prepare
    music_dir = Path(args.game_music_dir)
    mpf = _find_ci(music_dir, "ipod.mpf")
    if not mpf:
        print(f"error: ipod.mpf not found in {music_dir}")
        return 2
    out = Path(args.out) if args.out else music_dir / "custom"
    with tempfile.TemporaryDirectory() as td:
        idx = Path(td) / "ipod.s3fp"
        songs = _build_ipod_index(mpf, None, idx)
        prepare(songs, idx, Path(args.my_music), out, args.ffmpeg, args.fill, args.gain_db)
    print(f"\nprepared {out}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="skate3_music_import", description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    i = sub.add_parser("inspect", help="print MPF/MUS structure and per-sample codec info")
    i.add_argument("mpf"); i.add_argument("--mus")
    i.add_argument("--json", action="store_true")
    i.add_argument("--limit", type=int, default=40, help="rows to show (0 = all)")
    i.add_argument("--no-walk", action="store_true", help="skip SNS block walking (faster)")
    i.set_defaults(func=cmd_inspect)

    v = sub.add_parser("validate", help="consistency checks; exit 1 on errors")
    v.add_argument("mpf"); v.add_argument("--mus")
    v.add_argument("--track", type=int, default=0)
    v.set_defaults(func=cmd_validate)

    pr = sub.add_parser("probe", help="run vgmstream-cli -m on subsongs (optional dependency)")
    pr.add_argument("mpf"); pr.add_argument("--vgmstream")
    pr.add_argument("--subsong", type=int, action="append")
    pr.set_defaults(func=cmd_probe)

    b = sub.add_parser("backup", help="copy the music folder with SHA-256 manifest")
    b.add_argument("music_dir"); b.add_argument("--dest")
    b.set_defaults(func=cmd_backup)

    vf = sub.add_parser("verify", help="check a music folder against a backup manifest")
    vf.add_argument("music_dir"); vf.add_argument("backup_dir")
    vf.set_defaults(func=cmd_verify)

    ix = sub.add_parser("index", help="build the iPod XMA fingerprint index (S3FP)")
    ix.add_argument("mpf"); ix.add_argument("--mus")
    ix.add_argument("--out", default="ipod.s3fp")
    ix.set_defaults(func=cmd_index)

    pp = sub.add_parser("prepare", help="convert your music and lay it out for the runtime override")
    pp.add_argument("game_music_dir", help="...\\game\\data\\audio\\music (read only)")
    pp.add_argument("my_music", help="folder with your audio files (any format ffmpeg reads)")
    pp.add_argument("--out", help="output folder (default: <game_music_dir>\\custom)")
    pp.add_argument("--ffmpeg", help="path to ffmpeg if not on PATH")
    pp.add_argument("--fill", choices=["repeat", "original"], default="repeat",
                    help="slots left over when you have fewer than 46 tracks")
    pp.add_argument("--gain-db", type=float, default=0.0)
    pp.set_defaults(func=cmd_prepare)
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (MpfError, MusError) as exc:
        print(f"error: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
