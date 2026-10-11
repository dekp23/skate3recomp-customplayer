"""Read-only parser for EA EAAC-based .mus banks paired with Pathfinder v5.2/v5.3 maps.

Layouts follow vgmstream src/meta/ea_eaac_mpf_mus.c (MUS table) and
src/meta/ea_eaac.c (SNR header, SNS blocks). Nothing here writes files.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import List, Optional

CODECS = {
    0x00: "NONE", 0x01: "RESERVED", 0x02: "PCM16BE", 0x03: "EA-XMA", 0x04: "EA-XAS v1",
    0x05: "EALayer3 v1", 0x06: "EALayer3 v2 PCM", 0x07: "EALayer3 v2 Spike", 0x08: "GC DSP ADPCM",
    0x09: "EASpeex", 0x0A: "EATrax", 0x0B: "EA-MP3", 0x0C: "EAOpus", 0x0D: "EA-ATRAC9",
    0x0E: "EAOpus multistream", 0x0F: "EAOpus multistream uncoupled",
}
TYPES = {0: "RAM", 1: "STREAM", 2: "GIGASAMPLE"}

V5_2_TABLE = 0x08
V5_2_ENTRY = 0x0C
V5_3_TABLE = 0x28
V5_3_ENTRY = 0x1C


class MusError(ValueError):
    pass


@dataclass
class SnrHeader:
    version: int
    codec: int
    channels: int
    sample_rate: int
    type: int
    loop_flag: int
    num_samples: int
    loop_start: Optional[int]

    @property
    def codec_name(self) -> str:
        return CODECS.get(self.codec, f"unknown(0x{self.codec:X})")

    @property
    def type_name(self) -> str:
        return TYPES.get(self.type, f"unknown({self.type})")

    @property
    def duration_s(self) -> float:
        return self.num_samples / self.sample_rate if self.sample_rate else 0.0


@dataclass
class SnsInfo:
    blocks: int
    size: int
    terminated: bool
    error: Optional[str] = None


@dataclass
class MusEntry:
    index: int          # position in MUS table
    id_index: int       # "index" field
    sub_index: int
    snr_offset: int     # absolute byte offset
    sns_offset: int     # absolute byte offset (garbage for RAM sounds in v5.2)
    snr_size: Optional[int] = None   # v5.3 only
    sns_size: Optional[int] = None   # v5.3 only
    snr: Optional[SnrHeader] = None
    sns: Optional[SnsInfo] = None
    error: Optional[str] = None


@dataclass
class MusFile:
    layout: str          # "v5.2" or "v5.3"
    checksum: int        # first 4 bytes, big-endian
    declared_count: Optional[int]
    entries: List[MusEntry]
    size: int


def parse_snr(data: bytes, off: int) -> SnrHeader:
    if off + 8 > len(data):
        raise MusError(f"SNR offset 0x{off:X} beyond file end")
    h1, h2 = struct.unpack_from(">II", data, off)
    version = (h1 >> 28) & 0xF
    codec = (h1 >> 24) & 0xF
    channels = ((h1 >> 18) & 0x3F) + 1
    rate = h1 & 0x3FFFF
    typ = (h2 >> 30) & 0x3
    loop = (h2 >> 29) & 0x1
    nsamp = h2 & 0x1FFFFFFF
    loop_start = struct.unpack_from(">i", data, off + 8)[0] if loop and off + 12 <= len(data) else None
    if version not in (0, 1):
        raise MusError(f"SNR version {version} unknown (expected 0 or 1)")
    if rate == 0 or rate > 200000:
        raise MusError(f"implausible SNR sample rate {rate}")
    if typ not in TYPES:
        raise MusError(f"unknown SNR type {typ}")
    return SnrHeader(version, codec, channels, rate, typ, loop, nsamp, loop_start)


def walk_sns(data: bytes, off: int, max_blocks: int = 1_000_000) -> SnsInfo:
    """Walk EAAC v0 SNS blocks (id 0x00 data, 0x80 end). Size includes all blocks."""
    start, blocks = off, 0
    while off + 4 <= len(data) and blocks < max_blocks:
        word = struct.unpack_from(">I", data, off)[0]
        bid, bsize = word >> 24, word & 0xFFFFFF
        if bsize == 0 or bsize == 0xFFFFFF:
            return SnsInfo(blocks, off - start, False, f"bad block size at 0x{off:X}")
        if bid not in (0x00, 0x80):
            return SnsInfo(blocks, off - start, False, f"unexpected block id 0x{bid:02X} at 0x{off:X}")
        blocks += 1
        off += bsize
        if bid == 0x80:
            return SnsInfo(blocks, off - start, True)
    return SnsInfo(blocks, off - start, False, "no end block before EOF/limit")


def parse_mus(data: bytes, minor: int, endian: str, walk_streams: bool = True) -> MusFile:
    if len(data) < 8:
        raise MusError("file too small")
    checksum = struct.unpack_from(">I", data, 0)[0]
    entries: List[MusEntry] = []

    if minor == 2:
        if struct.unpack_from(endian + "I", data, 4)[0] != 0:
            raise MusError("v5.2 MUS: word at 0x04 is not zero (layout mismatch)")
        limit = len(data)
        i = 0
        while True:
            off = V5_2_TABLE + i * V5_2_ENTRY
            if off + V5_2_ENTRY > min(limit, len(data)):
                break
            idx, sub, snr, sns = struct.unpack_from(endian + "HHII", data, off)
            if idx == 0xFFFF:
                break
            if snr < off + V5_2_ENTRY or snr >= len(data):
                break  # table can't overlap data; treat as end
            limit = min(limit, snr)  # table ends before the first header it references
            entries.append(MusEntry(i, idx, sub, snr, sns))
            i += 1
        layout, declared = "v5.2", None
    elif minor == 3:
        declared = struct.unpack_from("<I", data, 4)[0]  # vgmstream: always little-endian
        if V5_3_TABLE + declared * V5_3_ENTRY > len(data):
            raise MusError(f"v5.3 MUS declares {declared} entries; table exceeds file")
        for i in range(declared):
            off = V5_3_TABLE + i * V5_3_ENTRY
            _chk, idx, sub, snr, sns, snr_sz, sns_sz = struct.unpack_from(endian + "IHHIIII", data, off)
            entries.append(MusEntry(i, idx, sub, snr * 0x10, sns * 0x80, snr_sz, sns_sz))
        layout = "v5.3"
    else:
        raise MusError(f"MPF minor version {minor}: only 5.2 and 5.3 MUS layouts are known")

    for ent in entries:
        try:
            ent.snr = parse_snr(data, ent.snr_offset)
            if walk_streams and ent.snr.type != 0:
                ent.sns = walk_sns(data, ent.sns_offset)
        except MusError as exc:
            ent.error = str(exc)
    return MusFile(layout, checksum, declared, entries, len(data))
