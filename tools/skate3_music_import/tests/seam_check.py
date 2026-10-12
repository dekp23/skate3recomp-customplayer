import numpy as np, wave
def read(i):
    w = wave.open(f"seg/{i+1}.wav","rb"); return np.frombuffer(w.readframes(w.getnframes()),"<i2").reshape(-1,2).astype(np.float64)
def lpc_extend(x, n, order=32, ctx=2048):
    h = x[-ctx:]; r = np.correlate(h, h, "full")[ctx-1:ctx+order]; r[0] *= 1.0001
    from numpy.linalg import solve
    R = np.array([[r[abs(i-j)] for j in range(order)] for i in range(order)]); a = solve(R, r[1:order+1])
    buf = list(h[-order:]); out = []
    for _ in range(n):
        v = float(np.dot(a, buf[::-1][:order])); out.append(v); buf.append(v); buf = buf[-order:]
    return np.array(out)
def join(parts, N):
    parts = [p.copy() for p in parts]
    if N:
        w = np.linspace(0, 1, N + 2)[1:-1]
        for k in range(1, len(parts)):
            for c in range(2):
                ext = lpc_extend(parts[k-1][:, c], N)
                parts[k][:N, c] = ext * (1 - w) + parts[k][:N, c] * w
    return parts
def metric(parts, N):
    x = np.concatenate(parts); d2 = np.abs(np.diff(x, n=2, axis=0)).max(axis=1); out = []
    for b in np.cumsum([len(p) for p in parts])[:-1]:
        local = np.concatenate([d2[max(0, b-4096):b-3], d2[b+N+2:b+4096]])
        out.append(d2[b-2:b+N+1].max() / (np.percentile(local, 99.9) + 1))
    return out
if __name__ == "__main__":
    for N in (0, 16, 32, 64):
        allr = []
        for song in range(46):
            allr += metric(join([read(song*30+k) for k in range(30)], N), N)
        a = np.array(allr)
        print(f"LPC crossfade N={N}: >1 {int((a>1).sum())}  >2 {int((a>2).sum())}  max {a.max():.2f}  median {np.median(a):.3f}")
