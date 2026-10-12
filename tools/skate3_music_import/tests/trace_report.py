"""Summarise MUSICTRACE lines from a skate3 log: song changes, gaps, skips, markers."""
import re, sys
kv = lambda s: dict(re.findall(r"(\w+)=(-?[\d.]+)", s))
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    if "USER LOG MARKER" in line:
        m = re.search(r"#(\d+).*t=([\d.]+)", line); print(f"--- F10 MARKER #{m.group(1)} at t={float(m.group(2))/1000:.1f}s")
    elif "MUSICTRACE" in line and " SONG " in line:
        d = kv(line.split("MUSICTRACE", 1)[1])
        t = float(d["t"]) / 1000; fp, fl = float(d["from_pos"]), float(d["from_len"])
        kind = "start" if d["from"] == "-1" else ("ENDED" if fl - fp < 15 else f"LEFT EARLY ({fp:.0f}/{fl:.0f}s)")
        print(f"t={t:8.1f}s song {d['from']:>3} -> {d['to']:>3}  {kind:<24} gap={float(d['gap_ms']):7.0f} ms  to_pos={float(d['to_pos']):.1f}s ctx={d['ctx']}")
    elif "MUSICTRACE" in line and " RESET " in line:
        d = kv(line.split("MUSICTRACE", 1)[1]); print(f"t={float(d['t'])/1000:8.1f}s reset ctx={d['ctx']} song={d['song']} pos={float(d['pos']):.1f}s")
