import io
import os
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))

from s3mus.cli import main, validate_pair  # noqa: E402
from s3mus.mpf import MpfError, parse_mpf  # noqa: E402
from s3mus.mus import parse_mus, parse_snr  # noqa: E402
import fixtures as fx  # noqa: E402

RATE = 44100


def two_streams():
    return [(fx.snr(num_samples=RATE * 2), fx.sns(3)),       # 2.000 s
            (fx.snr(num_samples=RATE * 3, channels=1), fx.sns(5))]  # 3.000 s


class SnrTests(unittest.TestCase):
    def test_bitfields(self):
        s = parse_snr(fx.snr(rate=48000, channels=6, codec=0x03, num_samples=12345, typ=1), 0)
        self.assertEqual((s.codec_name, s.sample_rate, s.channels, s.type_name, s.num_samples),
                         ("EA-XMA", 48000, 6, "STREAM", 12345))


class MpfTests(unittest.TestCase):
    def test_both_endians(self):
        for e in (">", "<"):
            m = parse_mpf(fx.build_mpf(e=e, samples=[(0, 2000), (1, 3000)]))
            self.assertEqual(m.endian, e)
            self.assertEqual(len(m.samples), 2)
            self.assertEqual(m.samples[1].duration_ms, 3000)
            self.assertEqual(m.tracks[0].mus_checksum, 0xC0FFEE11)

    def test_bad_magic(self):
        with self.assertRaises(MpfError):
            parse_mpf(b"NOPE" + b"\0" * 0x60)

    def test_track_ranges(self):
        m = parse_mpf(fx.build_mpf(track_starts=(0, 2, 0), checksums=(1, 2, 3),
                                   samples=[(0, 1)] * 5))
        self.assertEqual(m.samples_for_track(0), range(0, 2))
        self.assertEqual(m.samples_for_track(1), range(2, 5))
        self.assertEqual(m.samples_for_track(2), range(0))  # empty per vgmstream


class MusTests(unittest.TestCase):
    def _check(self, builder, minor, e):
        mus_b, layout = builder(e=e, streams=two_streams())
        mus = parse_mus(mus_b, minor, e)
        self.assertEqual(len(mus.entries), 2)
        for ent, (so, bo) in zip(mus.entries, layout):
            self.assertIsNone(ent.error)
            self.assertEqual((ent.snr_offset, ent.sns_offset), (so, bo))
            self.assertTrue(ent.sns.terminated)
        self.assertEqual(mus.entries[1].sns.blocks, 5)
        self.assertEqual(mus.entries[1].snr.channels, 1)
        return mus, layout

    def test_v52_be(self):
        self._check(fx.build_mus_v52, 2, ">")

    def test_v52_le(self):
        self._check(fx.build_mus_v52, 2, "<")

    def test_v53_be(self):
        self._check(fx.build_mus_v53, 3, ">")

    def test_corrupt_sns_detected(self):
        mus_b, layout = fx.build_mus_v52(streams=two_streams())
        b = bytearray(mus_b)
        b[layout[0][1]] = 0x33  # bad block id
        mus = parse_mus(bytes(b), 2, ">")
        self.assertFalse(mus.entries[0].sns.terminated)


class ValidateTests(unittest.TestCase):
    def test_pass_and_failures(self):
        mus_b, layout = fx.build_mus_v53(streams=two_streams())
        samples = [(bo // 0x80, d) for (_, bo), d in zip(layout, (2000, 3000))]
        mpf = parse_mpf(fx.build_mpf(minor=3, samples=samples))
        mus = parse_mus(mus_b, 3, ">")
        self.assertFalse([f for f in validate_pair(mpf, mus) if f.level != "INFO"])

        bad = parse_mpf(fx.build_mpf(minor=3, checksums=(0x12345678,),
                                     samples=[(samples[0][0], 9999), samples[1]]))
        findings = validate_pair(bad, mus)
        self.assertTrue(any(f.level == "ERROR" and f.message.startswith("checksum mismatch")
                            for f in findings))
        self.assertTrue(any(f.level == "WARN" and "MPF duration 9999 ms vs SNR 2000 ms" in f.message
                            for f in findings))


class EmptyTrackTests(unittest.TestCase):
    def test_empty_placeholder_track_is_not_an_error(self):
        # Mirrors real Skate 3 ipod.mpf: track 1 starts at 0 with checksum 0xF1F1F1F1.
        mus_b, layout = fx.build_mus_v53(streams=two_streams())
        samples = [(bo // 0x80, d) for (_, bo), d in zip(layout, (2000, 3000))]
        mpf = parse_mpf(fx.build_mpf(minor=3, track_starts=(0, 0),
                                     checksums=(0xC0FFEE11, 0xF1F1F1F1), samples=samples))
        mus = parse_mus(mus_b, 3, ">")
        findings = validate_pair(mpf, mus, track_index=1)
        self.assertFalse([f for f in findings if f.level == "ERROR"])
        self.assertTrue(any("is empty" in f.message for f in findings))


class CliTests(unittest.TestCase):
    def test_end_to_end_folder(self):
        with tempfile.TemporaryDirectory() as td:
            d = Path(td) / "music"
            d.mkdir()
            mus_b, layout = fx.build_mus_v52(streams=two_streams())
            (d / "ipod.mpf").write_bytes(fx.build_mpf(samples=[(0, 2000), (1, 3000)]))
            (d / "Ipod_Stream.mus").write_bytes(mus_b)

            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(main(["validate", str(d / "ipod.mpf")]), 0)
                self.assertEqual(main(["inspect", str(d / "ipod.mpf")]), 0)
                self.assertEqual(main(["backup", str(d), "--dest", str(Path(td) / "bk")]), 0)
                self.assertEqual(main(["verify", str(d), str(Path(td) / "bk")]), 0)
                (d / "Ipod_Stream.mus").write_bytes(mus_b + b"x")
                self.assertEqual(main(["verify", str(d), str(Path(td) / "bk")]), 1)
            out = buf.getvalue()
            self.assertIn("result: PASS", out)
            self.assertIn("EALayer3 v1", out)
            self.assertIn("1 file(s) differ", out)


if __name__ == "__main__":
    unittest.main()
