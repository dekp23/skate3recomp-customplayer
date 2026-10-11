import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from s3mus.prepare import Track, assign  # noqa: E402
from s3mus.xma import Song, fingerprint, group_songs  # noqa: E402


def songs(*secs):
    return [Song(i, i * 30, 30, int(s * 44100)) for i, s in enumerate(secs)]


def track(name, secs):
    return Track(Path(name), name + ".pcm", int(secs * 44100))


class GroupSongsTests(unittest.TestCase):
    def test_runs_with_one_sample_jitter(self):
        segs = [100, 101, 100] + [50] * 4 + [70]
        got = [(s.first_segment, s.num_segments, s.num_samples) for s in group_songs(segs)]
        self.assertEqual(got, [(0, 3, 301), (3, 4, 200), (7, 1, 70)])


class AssignTests(unittest.TestCase):
    def test_best_fit_without_truncation(self):
        a = assign(songs(300, 120, 200), [track("a", 110), track("b", 190)], fill="original")
        by_slot = {x.song: (x.track.source.name if x.track else None) for x in a}
        self.assertEqual(by_slot, {0: None, 1: "a", 2: "b"})

    def test_truncates_only_when_nothing_fits(self):
        a = assign(songs(100, 150), [track("long", 400)], fill="original")
        self.assertEqual(a[1].track.source.name, "long")  # longest slot
        self.assertIsNone(a[0].track)

    def test_repeat_fills_every_slot(self):
        a = assign(songs(100, 150, 300), [track("x", 140)], fill="repeat")
        self.assertTrue(all(x.track is not None for x in a))

    def test_more_tracks_than_slots(self):
        a = assign(songs(100), [track("x", 50), track("y", 90)], fill="original")
        self.assertEqual(a[0].track.source.name, "y")


class FingerprintTests(unittest.TestCase):
    def test_matches_cpp_reference(self):
        # Same vector as tests/cpp/test_custom_music.cpp TestFingerprintMatchesPython.
        self.assertEqual(fingerprint(bytes(range(256)) * 8), 0xB4DB28C285BFD025)


if __name__ == "__main__":
    unittest.main()
