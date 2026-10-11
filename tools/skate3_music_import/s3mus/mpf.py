"""Read-only parser for EA Pathfinder v5 map files (.mpf, magic "PFDx").

Layout follows vgmstream src/meta/ea_eaac_mpf_mus.c and MPFmaster's
PathFinderStructs.h. Endianness is detected from the magic: big-endian
files (Xbox 360 / PS3) store "PFDx" as-is, little-endian files store "xDFP".
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List

MAGIC = b"PFDx"
HEADER_SIZE = 0x48
TRACKINFO_SIZE = 0x14
SUBBANK_SIZE = 0x10
SAMPLE_ENTRY_SIZE = 0x08


class MpfError(ValueError):
    pass


@dataclass
class TrackInfo:
    index: int
    entry_offset: int
    starting_sample: int
    num_subbanks: int
    purge_mode: int
    mus_checksum: int  # read big-endian regardless of file endianness (as vgmstream does)
    max_aram: int
    max_mram: int

    @property
    def is_ram(self) -> bool:
        return self.num_subbanks != 0


@dataclass
class SampleEntry:
    index: int
    offset: int       # stream-type: table index or offset unit; RAM-type: (bank << 16) | sound
    duration_ms: int


@dataclass
class MpfFile:
    endian: str               # ">" big, "<" little
    major: int
    minor: int
    release: int
    prerelease: int
    project_id: int
    num_tracks: int
    num_sections: int
    num_events: int
    num_routers: int
    num_named_vars: int
    num_nodes: int
    offsets: dict
    tracks: List[TrackInfo] = field(default_factory=list)
    samples: List[SampleEntry] = field(default_factory=list)
    size: int = 0

    @property
    def endian_name(self) -> str:
        return "big" if self.endian == ">" else "little"

    def samples_for_track(self, track_index: int) -> range:
        """Global sample index range belonging to a track (vgmstream semantics)."""
        starts = [t.starting_sample for t in self.tracks]
        start = starts[track_index]
        if start == 0 and track_index != 0:
            return range(0)  # vgmstream treats this as an empty track
        later = [s for s in starts[track_index + 1:] if s > start]
        end = min(later) if later else len(self.samples)
        return range(start, end)


def parse_mpf(data: bytes) -> MpfFile:
    if len(data) < HEADER_SIZE:
        raise MpfError(f"file too small for Pathfinder header ({len(data)} bytes)")
    if data[:4] == MAGIC:
        e = ">"
    elif data[:4] == MAGIC[::-1]:
        e = "<"
    else:
        raise MpfError(f"bad magic {data[:4]!r}; expected 'PFDx' in either byte order")

    major, minor, release, prerelease = data[4], data[5], data[6], data[7]
    if major != 5:
        raise MpfError(f"Pathfinder major version {major}; only v5 is understood")

    (project_id, num_tracks, num_sections, num_events, num_routers, num_named_vars) = data[0x0C:0x12]
    (num_nodes,) = struct.unpack_from(e + "H", data, 0x12)
    names = ["nodeoffsets", "nodedata", "eventoffsets", "eventdata", "namedvars",
             "noderouters", "trackoffsets", "trackinfos", "sampleoffsets", "mapfilelen"]
    vals = struct.unpack_from(e + "10I", data, 0x14)
    offsets = dict(zip(names, vals))

    for k, v in offsets.items():
        if v > len(data):
            raise MpfError(f"header field {k}=0x{v:X} points past end of file (0x{len(data):X}); "
                           f"wrong endianness or not a v5 map")

    mpf = MpfFile(e, major, minor, release, prerelease, project_id, num_tracks, num_sections,
                  num_events, num_routers, num_named_vars, num_nodes, offsets, size=len(data))

    # Track table: num_tracks u32 entries, each a word offset (*4) to a PATHTRACKINFO.
    tt = offsets["trackoffsets"]
    for i in range(num_tracks):
        (word,) = struct.unpack_from(e + "I", data, tt + i * 4)
        off = word * 4
        if off + TRACKINFO_SIZE > len(data):
            raise MpfError(f"track {i} info offset 0x{off:X} out of range")
        start, nsub, purge = struct.unpack_from(e + "IHH", data, off)
        (chk,) = struct.unpack_from(">I", data, off + 8)
        aram, mram = struct.unpack_from(e + "II", data, off + 12)
        mpf.tracks.append(TrackInfo(i, off, start, nsub, purge, chk, aram, mram))

    # Sample table runs from sampleoffsets to mapfilelen in 8-byte entries.
    so, end = offsets["sampleoffsets"], offsets["mapfilelen"]
    if end < so:
        raise MpfError("mapfilelen precedes sampleoffsets")
    count = (end - so) // SAMPLE_ENTRY_SIZE
    for i in range(count):
        off, dur = struct.unpack_from(e + "II", data, so + i * SAMPLE_ENTRY_SIZE)
        mpf.samples.append(SampleEntry(i, off, dur))
    return mpf


def track_subbank_checksum(data: bytes, track: TrackInfo, bank: int) -> int:
    """Checksum for a RAM sub-bank (vgmstream reads entry+0x14+bank*0x10, big-endian)."""
    (v,) = struct.unpack_from(">I", data, track.entry_offset + 0x14 + bank * SUBBANK_SIZE)
    return v
