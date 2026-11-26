"""Dump objective / mission / operation tables of game build 25480438 from the unpacked image (RVA addressed)."""
import struct, sys, json
IMG = open(r"E:\File\Codes\0\Claude\26.11.1\HD2\darkfluidbackoprt\analysis\image\game-25480438.bin", "rb").read()
STR = r"E:\File\Codes\0\26.10.1\HD2\public_research_20261002\shalzuth_HelldiversData\data\translations\English.json"
strings = {int(k): v for k, v in json.load(open(STR, encoding="utf-8")).items()}


def rd(rva, n):
    return IMG[rva:rva + n]


OBJ, OBJ_N, OBJ_SZ = 0x32EF870, 153, 0xA0
MIS, MIS_N, MIS_SZ = 0x3773420, 162, 0x380
OPS, OPS_N, OPS_SZ = 0x32FEF10, 25, 0x490

out = {"objectives": [], "missions": [], "operations": []}
objname = {}
for d in range(OBJ_N):
    r = rd(OBJ + d * OBJ_SZ, OBJ_SZ)
    h, = struct.unpack_from("<I", r, 0)
    n, = struct.unpack_from("<I", r, 0x34)
    objname[h] = strings.get(n, "?")
    out["objectives"].append({"row": d, "hash": h, "name": objname[h], "w": struct.unpack_from("<III", r, 0x20)})
mission_row = {}
for m in range(MIS_N):
    r = rd(MIS + m * MIS_SZ, MIS_SZ)
    mid, = struct.unpack_from("<I", r, 0)
    mission_row[mid] = m
    ents = []
    for k in range(32):
        e = struct.unpack_from("<IfIIII", r, 0x40 + k * 0x18)
        if e[0]:
            ents.append({"objective": e[0], "name": objname.get(e[0], "?"), "w": e[1], "cls": e[3]})
    out["missions"].append({
        "row": m, "id": mid, "enabled": r[4], "faction": r[8], "range": struct.unpack_from("<II", r, 0xC),
        "kind": r[0x34], "name_hash": struct.unpack_from("<I", r, 0x340)[0],
        "name": strings.get(struct.unpack_from("<I", r, 0x340)[0], "?"),
        "head": r[:0x40].hex(), "objectives": ents})
for o in range(OPS_N):
    r = rd(OPS + o * OPS_SZ, OPS_SZ)
    ids = [struct.unpack_from("<If", r, 0x38 + 8 * k) for k in range(32)]
    out["operations"].append({
        "row": o, "id": struct.unpack_from("<I", r, 0)[0], "faction": r[8], "head": r[:0x38].hex(),
        "missions": [(mission_row.get(i, hex(i)), w) for i, w in ids if i],
        "tail_words": [struct.unpack_from("<I", r, 0x138 + 4 * k)[0] for k in range((OPS_SZ - 0x138) // 4)]})
json.dump(out, open(sys.argv[1], "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print("ok", len(out["missions"]), len(out["operations"]))
