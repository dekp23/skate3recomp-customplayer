"""Synthetic MPF/MUS builders. These encode the layouts as documented by vgmstream;
they are NOT real Skate 3 data and only prove the parser matches that documentation."""
import struct

HEADER_SIZE = 0x48


def snr(rate=44100, channels=2, codec=0x05, num_samples=44100, typ=1, version=0):
    h1 = (version << 28) | (codec << 24) | ((channels - 1) << 18) | rate
    h2 = (typ << 30) | num_samples
    return struct.pack(">II", h1, h2)


def sns(n_blocks=3, block_size=0x40):
    out = b""
    for i in range(n_blocks):
        bid = 0x80 if i == n_blocks - 1 else 0x00
        out += struct.pack(">I", (bid << 24) | block_size) + b"\x00" * (block_size - 4)
    return out


def build_mpf(e=">", minor=2, track_starts=(0,), checksums=(0xC0FFEE11,), samples=()):
    """samples: list of (offset, duration_ms). Returns bytes."""
    n_tracks = len(track_starts)
    trackoffsets = HEADER_SIZE
    trackinfos = trackoffsets + 4 * n_tracks
    trackinfos += (-trackinfos) % 4
    sampleoffsets = trackinfos + 0x14 * n_tracks
    mapfilelen = sampleoffsets + 8 * len(samples)

    hdr = bytearray(HEADER_SIZE)
    hdr[0:4] = b"PFDx" if e == ">" else b"xDFP"
    hdr[4], hdr[5] = 5, minor
    hdr[0x0D] = n_tracks
    struct.pack_into(e + "H", hdr, 0x12, 0)
    struct.pack_into(e + "10I", hdr, 0x14, *([HEADER_SIZE] * 6),
                     trackoffsets, trackinfos, sampleoffsets, mapfilelen)
    body = bytearray(mapfilelen - HEADER_SIZE)
    buf = hdr + body
    for i in range(n_tracks):
        off = trackinfos + 0x14 * i
        struct.pack_into(e + "I", buf, trackoffsets + 4 * i, off // 4)
        struct.pack_into(e + "IHH", buf, off, track_starts[i], 0, 0)
        struct.pack_into(">I", buf, off + 8, checksums[i])
    for i, (o, d) in enumerate(samples):
        struct.pack_into(e + "II", buf, sampleoffsets + 8 * i, o, d)
    return bytes(buf)


def build_mus_v52(e=">", checksum=0xC0FFEE11, streams=()):
    """streams: list of (snr_bytes, sns_bytes). Returns (bytes, [(snr_off, sns_off)])."""
    n = len(streams)
    table_end = 0x08 + 0x0C * n + 0x0C  # + terminator
    cursor = table_end + (-table_end) % 0x10
    layout, blob = [], bytearray()
    for s_hdr, s_body in streams:
        snr_off = cursor; cursor += len(s_hdr); cursor += (-cursor) % 0x80
        sns_off = cursor; cursor += len(s_body); cursor += (-cursor) % 0x10
        layout.append((snr_off, sns_off))
    buf = bytearray(cursor)
    struct.pack_into(">I", buf, 0, checksum)
    struct.pack_into(e + "I", buf, 4, 0)
    for i, ((so, bo), (s_hdr, s_body)) in enumerate(zip(layout, streams)):
        struct.pack_into(e + "HHII", buf, 0x08 + 0x0C * i, i, 0, so, bo)
        buf[so:so + len(s_hdr)] = s_hdr
        buf[bo:bo + len(s_body)] = s_body
    struct.pack_into(e + "HHII", buf, 0x08 + 0x0C * n, 0xFFFF, 0xFFFF, 0, 0)
    return bytes(buf), layout


def build_mus_v53(e=">", checksum=0xC0FFEE11, streams=()):
    n = len(streams)
    cursor = 0x28 + 0x1C * n
    cursor += (-cursor) % 0x80
    layout = []
    for s_hdr, s_body in streams:
        cursor += (-cursor) % 0x10; snr_off = cursor; cursor += len(s_hdr)
        cursor += (-cursor) % 0x80; sns_off = cursor; cursor += len(s_body)
        layout.append((snr_off, sns_off))
    buf = bytearray(cursor)
    struct.pack_into(">I", buf, 0, checksum)
    struct.pack_into("<I", buf, 4, n)
    for i, ((so, bo), (s_hdr, s_body)) in enumerate(zip(layout, streams)):
        struct.pack_into(e + "IHHIIII", buf, 0x28 + 0x1C * i, 0xDEADBEEF, i, 0,
                         so // 0x10, bo // 0x80, len(s_hdr), len(s_body))
        buf[so:so + len(s_hdr)] = s_hdr
        buf[bo:bo + len(s_body)] = s_body
    return bytes(buf), layout
