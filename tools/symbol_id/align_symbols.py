"""Name unknown SDK/runtime functions in our symbols.txt by aligning against reference decomps.

Anchors = function names present (uniquely) in both our symbols and a reference.
Between two consecutive anchors that appear in the same order in both, if the run of
functions has the same count and every size matches exactly, the reference names are adopted.
Then repeat with newly named functions as anchors until nothing changes.
"""
import re, sys, json, collections

LINE = re.compile(r"^(\S+) = \.text:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")

def load(path):
    out = []
    for l in open(path, encoding="utf-8", errors="replace"):
        m = LINE.match(l)
        if m:
            out.append([m.group(1), int(m.group(2), 16), int(m.group(3), 16)])
    out.sort(key=lambda x: x[1])
    return out

def generic(n):
    return re.match(r"^(fn|func|lbl|sub|FUN|auto|gap)_", n) is not None

ours_path, *refs = sys.argv[1:]
ours = load(ours_path)
names = [f[0] if not generic(f[0]) else None for f in ours]
orig_named = sum(n is not None for n in names)
src = {}  # index -> reference that named it

ref_lists = {}
for rp in refs:
    r = load(rp)
    cnt = collections.Counter(f[0] for f in r)
    ref_lists[rp] = (r, cnt)

changed = True
rounds = 0
while changed:
    changed = False; rounds += 1
    for rp, (r, rcnt) in ref_lists.items():
        ridx = {f[0]: i for i, f in enumerate(r) if rcnt[f[0]] == 1 and not generic(f[0])}
        ocnt = collections.Counter(n for n in names if n)
        anchors = [(i, ridx[n]) for i, n in enumerate(names) if n and ocnt[n] == 1 and n in ridx]
        for (a_o, a_r), (b_o, b_r) in zip(anchors, anchors[1:]):
            if b_r <= a_r:
                continue
            go, gr = list(range(a_o + 1, b_o)), list(range(a_r + 1, b_r))
            if not go or len(go) != len(gr):
                continue
            # anchors themselves must have matching sizes (confirms same version of the code)
            if len(go) > 2500 or len(gr) > 2500:
                continue
            # global alignment of size sequences inside the gap
            n_, m_ = len(go), len(gr)
            sc = [[0]*(m_+1) for _ in range(n_+1)]; bt = [[0]*(m_+1) for _ in range(n_+1)]
            for i in range(1, n_+1): sc[i][0] = -2*i; bt[i][0] = 1
            for j in range(1, m_+1): sc[0][j] = -2*j; bt[0][j] = 2
            for i in range(1, n_+1):
                for j in range(1, m_+1):
                    d = sc[i-1][j-1] + (3 if ours[go[i-1]][2] == r[gr[j-1]][2] else -1)
                    u = sc[i-1][j] - 2; l = sc[i][j-1] - 2
                    sc[i][j], bt[i][j] = max((d,0),(u,1),(l,2))
            pairs = []; i, j = n_, m_
            while i > 0 and j > 0:
                if bt[i][j] == 0: pairs.append((go[i-1], gr[j-1])); i -= 1; j -= 1
                elif bt[i][j] == 1: i -= 1
                else: j -= 1
            pairs.reverse()
            ex = [ours[a][2] == r[b][2] for a, b in pairs]
            for k, (a, b) in enumerate(pairs):
                # sizes equal, or a mismatch sandwiched between exact neighbours (same function, changed body)
                ok = ex[k] or (0 < k < len(pairs)-1 and ex[k-1] and ex[k+1]
                               and pairs[k-1][0] == a-1 and pairs[k+1][0] == a+1
                               and pairs[k-1][1] == b-1 and pairs[k+1][1] == b+1)
                n = r[b][0]
                if ok and names[a] is None and not generic(n) and n not in ocnt:
                    # require at least one exact neighbour so isolated size coincidences are ignored
                    nb = (k > 0 and ex[k-1] and pairs[k-1][0] == a-1) or (k < len(pairs)-1 and ex[k+1] and pairs[k+1][0] == a+1)
                    if nb or len(go) == 1:
                        names[a] = n; src[a] = rp.split("/")[-1].split(".")[0]; changed = True; ocnt[n] += 1

new = {ours[i][1]: (names[i], src[i]) for i in src}
print(f"rounds {rounds}; originally named {orig_named}; newly named {len(new)}; total {orig_named+len(new)} / {len(ours)}")
json.dump({f"{a:08X}": v for a, v in sorted(new.items())}, open(sys.argv[1] + ".matches.json", "w"), indent=0)
by = collections.Counter(v[1] for v in new.values())
print("by reference:", dict(by))
