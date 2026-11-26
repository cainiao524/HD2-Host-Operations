"""Dump the game's own text (stingray .strings) for English (us), Traditional (tc) and Simplified Chinese (cn; FileDiver calls it sc).

python -I dump_strings.py <game data dir> <out dir>   ->  strings-us.json, strings-tc.json, strings-cn.json ({id: text})
Format as FileDiver reads it (stingray/strings/strings.go): magic, version, count, language thin hash, ids[count],
offsets[count], NUL-terminated UTF-8 texts.
"""
import json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hd2data import DataDir, h64, murmur64

data_dir, out_dir = sys.argv[1], sys.argv[2]
d = DataDir(data_dir)
strings_type = h64("strings")
want = {murmur64(code.encode()) >> 32: code for code in ("us", "tc", "cn")}
tables = {code: {} for code in want.values()}
files = 0
for (typ, name) in list(d.index):
    if typ != strings_type: continue
    raw = d.read(typ, name)
    if not raw or len(raw) < 16: continue
    magic, version, count = struct.unpack_from("<4sII", raw, 0)
    if not count: continue
    language, = struct.unpack_from("<I", raw, 12)
    code = want.get(language)
    if not code: continue
    files += 1
    ids = struct.unpack_from(f"<{count}I", raw, 16)
    offsets = struct.unpack_from(f"<{count}I", raw, 16 + 4 * count)
    for i, o in zip(ids, offsets):
        end = raw.index(b"\0", o)
        tables[code][i] = raw[o:end].decode("utf-8", "replace")
for code, table in tables.items():
    json.dump({str(k): v for k, v in sorted(table.items())}, open(os.path.join(out_dir, f"strings-{code}.json"), "w",
              encoding="utf-8"), ensure_ascii=False)
    print(code, len(table))
print("files", files)
