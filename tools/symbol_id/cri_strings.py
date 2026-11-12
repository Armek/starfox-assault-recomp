"""Name CRI middleware functions from the error strings they reference.

CRI libraries embed messages like "E0404: ADXT_Create: parameter error" or
"1060102: Internal Error: adxm_goto_mwidle_border". If a function is the only one that
references strings naming identifier X, and X looks like a function name, name it X.
"""
import re, glob, sys, collections, json

asm_dir, out = sys.argv[1], sys.argv[2]
strings = {}
fn_refs = collections.defaultdict(set)
for path in glob.glob(f"{asm_dir}/*.s"):
    cur_obj = None; cur_fn = None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith(".obj "): cur_obj = line.split()[1].rstrip(",")
        elif line.startswith(".fn "): cur_fn = line.split()[1].rstrip(",")
        elif line.startswith(".endfn"): cur_fn = None
        elif cur_obj and line.strip().startswith(".string "):
            strings.setdefault(cur_obj, "")
            strings[cur_obj] += line.strip()[8:].strip('"')
        elif line.startswith(".endobj"): cur_obj = None
        if cur_fn:
            for m in re.finditer(r"\b(lbl_[0-9A-F]{8})(?:\+0x([0-9A-F]+))?", line):
                fn_refs[cur_fn].add(m.group(1))

# String tables can be merged into one object; split on embedded NULs is not available here, so
# match identifiers anywhere in the referenced string object.
ID = re.compile(r"\b((?:ADX|adx|ADXT|ADXF|ADXM|ADXSJD|ADXCRS|ADXB|ADXPD|AHX|ahx|MWSFD|mwSfd|mwsfd|SFD|sfd|SFH|sfh|SFX|sfx|MPV|mpv|MPS|mps|SJ|sj|SJRBF|SJMEM|SJUNI|CVFS|cvFs|cvfs|DVCI|dvci|MFCI|mfci|CFT|cft|DCT|dct|SUD|sud|LSC|lsc|SVM|svm|MW|mw)[A-Za-z0-9]*_[A-Za-z0-9_]+)\b")
cand = collections.defaultdict(set)      # function -> identifiers named in its strings
users = collections.defaultdict(set)     # identifier -> functions referencing it
for fn, refs in fn_refs.items():
    for r in refs:
        s = strings.get(r)
        if not s: continue
        for m in ID.finditer(s):
            ident = m.group(1).rstrip("_")
            cand[fn].add(ident); users[ident].add(fn)

result = {}
for fn, ids in cand.items():
    if not re.match(r"^fn_", fn): continue
    uniq = [i for i in ids if len(users[i]) == 1]
    if len(uniq) == 1:
        result[fn] = uniq[0]
names = collections.Counter(result.values())
result = {f: n for f, n in result.items() if names[n] == 1}
json.dump(result, open(out, "w"), indent=0)
print(f"string objects {len(strings)}; functions with CRI identifiers {len(cand)}; named {len(result)}")
for f, n in list(sorted(result.items()))[:25]: print("  ", f, "->", n)
