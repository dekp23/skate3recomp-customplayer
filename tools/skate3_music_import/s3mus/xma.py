"""EA-XMA packet extraction, song grouping and fingerprint index.

EA-XMA SNS block layout (vgmstream src/meta/ea_eaac_streamfile.h, codec 0x03):
    0x00 u32be  block id (8 bits) | block size (24 bits)
    0x04 u32be  samples in this block
    0x08        per XMA stream: u32be (size * 4), where size includes this 4-byte
                mini header, followed by XMA packet data
Measured on Skate 3 X360 Ipod_Stream.mus: one stereo stream, one XMA packet per
block, trimmed to < 2048 bytes (the game must pad it back for the XMA hardware).

Fingerprint (must match src/skate3_custom_music.cpp):
    FNV-1a 64 over packet bytes [4, 4 + 256)  -- skips the 4-byte XMA packet header,
    which the game or runtime may rewrite, and stays clear of the trimmed tail.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Dict, Iterator, List, Tuple

from .mus import MusFile

FP_SKIP = 4
FP_LEN = 256
INDEX_MAGIC = b"S3FP"
INDEX_VERSION = 4
SAMPLES_PER_FRAME = 512

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF


def fnv1a64(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK64
    return h


def fingerprint(packet: bytes) -> int:
    if len(packet) < FP_SKIP + FP_LEN:
        raise ValueError(f"packet too short for fingerprint ({len(packet)} bytes)")
    return fnv1a64(packet[FP_SKIP:FP_SKIP + FP_LEN])


class XmaError(ValueError):
    pass


def packet_frame_count(packet: bytes) -> int:
    """Frames that begin in this XMA packet (header bits 31..26), as rexglue's
    xma::GetPacketFrameCount reads them."""
    return packet[0] >> 2


@dataclass
class Packet:
    segment: int
    index: int          # packet number within segment
    sample_start: int   # first output sample of this block within the segment
    samples: int        # samples the block declares
    data: bytes


def iter_segment_packets(data: bytes, segment: int, sns_offset: int) -> Iterator[Packet]:
    off, idx, sample_pos = sns_offset, 0, 0
    while off + 12 <= len(data):
        word, nsamp = struct.unpack_from(">II", data, off)
        bid, bsize = word >> 24, word & 0xFFFFFF
        if bsize < 12 or bid not in (0x00, 0x80):
            raise XmaError(f"segment {segment}: bad SNS block id 0x{bid:02X} size {bsize} at 0x{off:X}")
        stream_size = struct.unpack_from(">I", data, off + 8)[0] // 4
        if stream_size < 4 or 8 + stream_size > bsize:
            raise XmaError(f"segment {segment}: stream size {stream_size} exceeds block at 0x{off:X} "
                           f"(multi-stream EA-XMA not supported)")
        yield Packet(segment, idx, sample_pos, nsamp, data[off + 12: off + 8 + stream_size])
        sample_pos += nsamp
        idx += 1
        off += bsize
        if bid == 0x80:
            return
    raise XmaError(f"segment {segment}: no end block")


@dataclass
class Song:
    index: int
    first_segment: int
    num_segments: int
    num_samples: int

    @property
    def seconds(self) -> float:
        return self.num_samples / 44100.0


def group_songs(segment_samples: List[int], tolerance: int = 1) -> List[Song]:
    """Group consecutive segments of equal length (+/- tolerance samples) into songs.

    Inference from the real Skate 3 Ipod_Stream.mus: 1380 segments form exactly 46 runs
    of 30 equal-length segments, consistent with 46 songs each split into 30 parts.
    Not yet cross-checked against the MPF node graph.
    """
    songs: List[Song] = []
    i = 0
    while i < len(segment_samples):
        j, ref = i + 1, segment_samples[i]
        while j < len(segment_samples) and abs(segment_samples[j] - ref) <= tolerance:
            j += 1
        songs.append(Song(len(songs), i, j - i, sum(segment_samples[i:j])))
        i = j
    return songs


@dataclass
class IndexStats:
    segments: int
    songs: int
    packets: int
    unique: int
    dup_hashes: int
    short_packets: int
    sample_mismatch: List[Tuple[int, int, int]]  # (segment, declared, summed)


def build_index(data: bytes, mus: MusFile):
    """Returns (entries, songs, seg_samples_by_song, stats).

    entries: sorted (hash, song, segment_in_song, offset) where offset is the position of
    the packet's first frame within its segment's audible timeline (frames_before * 512 -
    lead; the first packet is negative by the decoder delay)."""
    seg_samples = []
    for ent in mus.entries:
        if ent.snr is None or ent.snr.codec != 0x03:
            raise XmaError(f"segment {ent.index} is not EA-XMA")
        seg_samples.append(ent.snr.num_samples)
    songs = group_songs(seg_samples)
    seg_to_song: Dict[int, Tuple[int, int]] = {}
    for s in songs:
        for k in range(s.num_segments):
            seg_to_song[s.first_segment + k] = (s.index, k)

    entries = []
    seen: Dict[int, int] = {}
    dups = shorts = 0
    mismatch = []
    for ent in mus.entries:
        song, k = seg_to_song[ent.index]
        packets = list(iter_segment_packets(data, ent.index, ent.sns_offset))
        # Decoder delay: the first packet decodes more samples than its block declares
        # (measured: 384 on every Skate 3 iPod segment).
        lead = packet_frame_count(packets[0].data) * SAMPLES_PER_FRAME - packets[0].samples
        total = 0
        frames_before = 0
        for p in packets:
            total += p.samples
            off = frames_before * SAMPLES_PER_FRAME - lead
            frames_before += packet_frame_count(p.data)
            if len(p.data) < FP_SKIP + FP_LEN:
                shorts += 1
                continue
            h = fingerprint(p.data)
            if h in seen:
                dups += 1
                continue  # keep first occurrence; duplicates are ambiguous
            seen[h] = 1
            entries.append((h, song, k, off))
        if total != ent.snr.num_samples:
            mismatch.append((ent.index, ent.snr.num_samples, total))
    entries.sort()
    by_song = [seg_samples[s.first_segment:s.first_segment + s.num_segments] for s in songs]
    stats = IndexStats(len(mus.entries), len(songs), len(entries) + dups, len(seen), dups, shorts, mismatch)
    return entries, songs, by_song, stats


def write_index(path, entries, by_song) -> None:
    """Little-endian binary index (v4):
        char[4] 'S3FP'; u32 version=4; u32 fp_skip; u32 fp_len; u32 num_songs; u32 num_entries;
        per song: u32 num_segments; u32 segment_samples[num_segments]
        { u64 hash; u16 song; u16 segment; i32 offset_in_segment; } entries[num_entries]
        (sorted by hash)
    """
    with open(path, "wb") as f:
        f.write(INDEX_MAGIC)
        f.write(struct.pack("<5I", INDEX_VERSION, FP_SKIP, FP_LEN, len(by_song), len(entries)))
        for segs in by_song:
            f.write(struct.pack(f"<I{len(segs)}I", len(segs), *segs))
        for h, song, seg, off in entries:
            f.write(struct.pack("<QHHi", h, song, seg, off))


def read_index(path):
    """Returns (segment_samples_by_song, entries)."""
    raw = open(path, "rb").read()
    if raw[:4] != INDEX_MAGIC:
        raise XmaError("not an S3FP index")
    ver, skip, ln, nsong, nent = struct.unpack_from("<5I", raw, 4)
    if (ver, skip, ln) != (INDEX_VERSION, FP_SKIP, FP_LEN):
        raise XmaError(f"index parameters {ver},{skip},{ln} unsupported")
    off = 24
    by_song = []
    for _ in range(nsong):
        (n,) = struct.unpack_from("<I", raw, off)
        by_song.append(list(struct.unpack_from(f"<{n}I", raw, off + 4)))
        off += 4 + 4 * n
    ents = [struct.unpack_from("<QHHi", raw, off + 16 * i) for i in range(nent)]
    return by_song, ents
