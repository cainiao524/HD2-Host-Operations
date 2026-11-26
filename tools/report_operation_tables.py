"""Write the operation/mission report (UTF-8 Markdown) from tables.json and Havoc's name lists."""
import json, sys
D = "F:/Game/A/HD2/HellForge/Havoc/files/dropdowns/"
t = json.load(open(sys.argv[1], encoding="utf-8"))
out_path = sys.argv[2]


def hashmap(name):
    out = {}
    for line in open(D + name, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line or line.startswith("//") or ":" not in line: continue
        k, v = line.split(":", 1)
        if k.strip().isdigit(): out[int(k)] = v.strip()
    return out


mname = hashmap("mission_name_hash.txt")
oname = hashmap("operation_types.txt")
FAC = {2: "蟲族", 3: "機器人", 4: "光能者"}
MODE = {1: "一般", 2: "Horde", 3: "防禦/短任務", 4: "Opportunity", 5: "教學", 6: "閃擊", 7: "幽暗遠征", 8: "突擊隊"}
OPKIND = {0: "一般", 4: "防禦", 8: "守城", 9: "奪城", 12: "巨型工廠", 13: "賽博斯坦"}
miss = t["missions"]


def label(m):
    n = mname.get(m["id"])
    if not n or n == "None": n = m["name"] if m["name"] != "?" else "（無名稱）"
    return n


used = {}
for o in t["operations"]:
    for row, w in o["missions"]:
        used.setdefault(row, []).append(o["row"])

L = []
L.append("# 任務組與隱藏任務（遊戲版本 25480438）\n")
L.append("來源：遊戲記憶體映像 `analysis/image/game-25480438.bin`。任務組表 `game.dll+0x32FEF10`（25 列 × 0x490），"
         "任務表 `+0x3773420`（162 列 × 0x380），目標表 `+0x32EF870`（153 列）。"
         "名稱取自 Havoc 的清單（以 ID 對應）與 English.json；任務組名稱是 Havoc 依列號標的，可能來自其他遊戲版本。"
         "產生腳本：`analysis/dump_operation_tables.py <tables.json>` 再 `analysis/report_operation_tables.py <tables.json> <輸出.md>`。\n")
L.append("權重說明：「固定」= 權重 1e30，該任務組必定包含這個任務；其他數字是抽選權重，沒寫的是 1。\n")
L.append("## 25 個任務組\n")
L.append("| 列 | 陣營 | 啟用 | 類型 | 名稱（Havoc） | 任務組 ID | 任務（名稱[任務列]） |")
L.append("|---|---|---|---|---|---|---|")
for o in t["operations"]:
    head = bytes.fromhex(o["head"])
    parts = []
    for row, w in o["missions"]:
        m = miss[row]
        tag = "固定" if w > 1e20 else (f"權重 {w:g}" if w != 1.0 else "")
        extra = [x for x in (tag, f"難度 {m['range'][0]}–{m['range'][1]}" if tuple(m["range"]) != (1, 11) else "") if x]
        parts.append(f"{label(m)}[{row}{'，' + '，'.join(extra) if extra else ''}]")
    L.append(f"| {o['row']} | {FAC.get(o['faction'], o['faction'])} | {'是' if head[4] else '**否**'} | "
             f"{OPKIND.get(head[0x14], head[0x14])} | {oname.get(o['row'], '?')} | {o['id']} | {'、'.join(parts)} |")

L.append("\n## 任何任務組都沒用到的任務\n")
L.append(f"任務表 162 列，任務組用到 {len(used)} 列，以下 {162 - len(used)} 列沒有任何任務組會抽到。"
         "「啟用」是任務列自己的開關（+4）。\n")
for enabled, title in ((1, "### 啟用中，但沒有任務組調用\n"), (0, "### 已停用\n")):
    L.append(title)
    L.append("| 任務列 | 陣營 | 模式 | 難度 | 任務 ID | 名稱 | 目標 |")
    L.append("|---|---|---|---|---|---|---|")
    for m in miss:
        if m["row"] in used or m["enabled"] != enabled: continue
        objs = sorted({e["name"] for e in m["objectives"] if e["name"] != "?"})
        L.append(f"| {m['row']} | {FAC.get(m['faction'], m['faction'])} | {MODE.get(m['kind'], m['kind'])} | "
                 f"{m['range'][0]}–{m['range'][1]} | {m['id']} | {label(m)} | {'、'.join(objs) or '—'} |")
    L.append("")
open(out_path, "w", encoding="utf-8").write("\n".join(L) + "\n")
print("written", out_path)
