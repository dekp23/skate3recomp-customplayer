"""Writes Python reference results for C++ library::Assign (tests/cpp/test_library.cpp).
usage: python gen_assign_cases.py OUT.txt"""
import random, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from s3mus.prepare import assign, Track  # noqa: E402
from s3mus.xma import Song  # noqa: E402

random.seed(7)
lines = []
for _ in range(500):
    songs = [Song(i, 0, 1, random.choice([random.randint(1, 50), random.randint(1, 5)]))
             for i in range(random.randint(1, 12))]
    tracks = [Track(Path(f"t{j}"), f"{j}.pcm", random.choice([random.randint(1, 60), random.randint(1, 5)]))
              for j in range(random.randint(0, 15))]
    fill = random.choice([True, False])
    a = assign(songs, tracks, "repeat" if fill else "original")
    exp = [int(x.track.pcm_name.split(".")[0]) if x.track else -1 for x in a]
    lines.append("%s|%s|%d|%s" % (",".join(str(s.num_samples) for s in songs),
                                  ",".join(str(t.num_samples) for t in tracks), int(fill),
                                  ",".join(map(str, exp))))
Path(sys.argv[1]).write_text("\n".join(lines) + "\n")
print(len(lines), "cases")
