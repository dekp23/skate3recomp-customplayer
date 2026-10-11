"""Generate an XMA packet stream for the C++ custom-music engine test.

Replays real packets the way the XMA hardware sees them (each EA block's trimmed packet
padded to 2048 bytes) on several interleaved contexts, with expected results.

Record format (little-endian), repeated:
    u32 context; i32 expected_song (-1 = must pass through); i32 frame0_pos;
    u32 frame_count; u32 flags (1 = reset context before this packet; 2 = short packet);
    u8  packet[2048]

usage: python sim_stream.py IPOD.mpf GAME.mpf OUT.bin
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from s3mus.cli import resolve_pair  # noqa: E402
from s3mus.mpf import parse_mpf  # noqa: E402
from s3mus.mus import parse_mus  # noqa: E402
from s3mus.xma import (FP_LEN, FP_SKIP, SAMPLES_PER_FRAME, group_songs,  # noqa: E402
                       iter_segment_packets, packet_frame_count)

PACKET = 2048


def load(mpf_path):
    mpf_path = Path(mpf_path)
    mus_path = resolve_pair(mpf_path, None)
    data = mus_path.read_bytes()
    mpf = parse_mpf(mpf_path.read_bytes())
    return data, parse_mus(data, mpf.minor, mpf.endian, walk_streams=False)


def song_packets(data, mus, song, max_segments=None):
    seg_samples = [e.snr.num_samples for e in mus.entries]
    s = group_songs(seg_samples)[song]
    base = 0
    segs = range(s.first_segment, s.first_segment + s.num_segments)
    if max_segments:
        segs = list(segs)[:max_segments]
    for k in segs:
        pk = list(iter_segment_packets(data, k, mus.entries[k].sns_offset))
        lead = packet_frame_count(pk[0].data) * SAMPLES_PER_FRAME - pk[0].samples
        fb = 0
        for p in pk:
            yield p.data, song, base + fb * SAMPLES_PER_FRAME - lead
            fb += packet_frame_count(p.data)
        base += seg_samples[k]


def game_packets(data, mus, segments):
    for k in segments:
        for p in iter_segment_packets(data, k, mus.entries[k].sns_offset):
            yield p.data, -1, 0


def record(ctx, song, pos, pkt, flags=0):
    if len(pkt) < FP_SKIP + FP_LEN:
        flags |= 2
    padded = pkt[:PACKET].ljust(PACKET, b"\0")
    return struct.pack("<IiiII", ctx, song, pos, packet_frame_count(pkt), flags) + padded


def main(ipod_mpf, game_mpf, out):
    idata, imus = load(ipod_mpf)
    gdata, gmus = load(game_mpf)
    streams = {
        1: list(song_packets(idata, imus, 8)),                 # assigned, full song
        2: list(song_packets(idata, imus, 16)),                # assigned, interleaved
        3: list(game_packets(gdata, gmus, range(0, 6))),       # other music: pass through
        4: list(song_packets(idata, imus, 0, max_segments=2)),  # unassigned: pass through
    }
    recs = []
    longest = max(len(v) for v in streams.values())
    for i in range(longest):
        for ctx, pk in streams.items():
            if i < len(pk):
                data, song, pos = pk[i]
                recs.append(record(ctx, song, pos, data))
    # Context 1 reused for game audio after a reset: must pass through from frame 0.
    first = True
    for data, song, pos in game_packets(gdata, gmus, range(10, 12)):
        recs.append(record(1, song, pos, data, flags=1 if first else 0))
        first = False
    Path(out).write_bytes(b"".join(recs))
    print(f"wrote {len(recs)} packet records to {out}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
