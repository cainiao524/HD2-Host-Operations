# 交接：local.host-ops-test（自訂星球與任務組）

最後更新：2026-11-26，版本 `0.3.0-test21`（已安裝到 HellForge）。

## 目標與硬性要求

- **純主機** HellForge 模組（SDK 16，遊戲構建 **25480438**）。房主可在任意星球自訂：入口星球、地形、任務組（行動範本）、任務 1–3、星球詞條（含變種、DSS 飛鷹風暴等）。
- **沒裝模組的客機要能加入並同步**：要相容 8 人（8PM），也要支援中途加入。
- 遊戲內選單要像 HellForge 的 F8 編輯器，用 **F11** 開關，用 Dear ImGui 畫在 D3D12 上，**不要 Win32 視窗**。
- 關閉選單後，遊戲原本的滑鼠和視角邏輯必須完全恢復，F8 也一樣。
- 模組不更動也不限制難度，難度交給戰術桌決定。
- 選單提供繁體中文、簡體中文、英文三種語言。任務組、任務、陣營、星球和詞條的名稱，優先用**遊戲本身的文本**。

## 建置與安裝

```powershell
cd E:\File\Codes\0\Claude\26.11.1\HD2\darkfluidbackoprt\host-ops-test
python build.py --hf-root "F:\Game\A\HD2\HellForge" --warnings-as-errors
```
```powershell
& "F:\Game\A\HD2\HellForge\app\cli\HellForge.Cli.exe" status
```
```powershell
& "F:\Game\A\HD2\HellForge\app\cli\HellForge.Cli.exe" mod-add --file dist\host-ops-test-<版本>.zip
```

- **編譯工具：** Zig 0.17，在 `HellForge\tools\zig`。Dear ImGui 固定用 **1.91.8**（`third_party/imgui-1.91.8`，程式裡有 static_assert 鎖版本）。
- **改版本號：** `hellforge-mod.json` 的 `version`，加上 `src/operations.c` 啟動日誌裡的 `(testNN: in-game ...)`。
- **安裝規則：** 只在 `status` 回報 `session.running=false` 時安裝。遊戲執行中，就開一個背景迴圈每 20 秒查一次，等遊戲關掉再裝。
- **日誌位置：** `F:\Game\A\HD2\HellForge\logs\sessions\<最新>\mod-local.host-ops-test.log`，時間是 UTC。
- **戰術桌送出的請求：** 在同一個資料夾的 `backend\mission-bodies\*.json`，包括 Operation_Create、Mission_Start、Abandon 等。

## 原始碼

| 檔案 | 內容 |
|---|---|
| `src/operations.c` | 遊戲邏輯：所有 hook、戰況寫入、行動、任務、日誌、預設檔的套用 |
| `src/overlay.cpp` | D3D12 疊加層（Present、Present1、ResizeBuffers、ExecuteCommandLists）、ImGui 初始化、輸入處理 |
| `src/host_ui.cpp` | ImGui 選單，分頁：入口星球／地形來源／行動設定／星球詞條／自定義任務 |
| `src/menu.c/.h` | 預設資料 `OperationPreset`、預設檔和語言檔。HellForge 會改寫遊戲的 LOCALAPPDATA，所以實際位置是 `F:\Game\A\HD2\HellForge\userdata\environment\Local\HellForge\HostOperationsTest\presets.ini` 和 `language.txt` |
| `src/i18n.c/.h` | 介面文字（繁體和英文兩欄；簡體在執行時用 `LCMapStringEx` 轉換，再加大陸用語替換表）；`loc()`、`tr()` |
| `src/catalog_data.c` | 星球 274 顆、地形 438 份、變種、詞條。由 build_catalog.py 從 25480438 的 generated_planet_data 產生，產生器不在這個資料夾 |
| `src/mission_catalog.c` | 任務表 162 列，由 `tools/gen_mission_catalog.py` 產生 |
| `src/game_text.c/.h` | 遊戲官方的繁／英／簡名稱，由 `tools/gen_game_text.py` 產生 |

### 分析工具（`tools/`）
- **game.dll 映像：** 讀資料表要用執行中的 game.dll 記憶體 dump（可用 RVA 定址），硬碟上的 game.dll 是加殼的。映像不在這個倉庫裡。
- **`dump_operation_tables.py` → `report_operation_tables.py`：** 產生 任務組報告（25 個任務組，以及沒被任何任務組用到的任務）。
- **`dump_strings.py`：** 取出遊戲文本，存成 `strings-{us,tc,cn}.json`（遊戲文本，不在這個倉庫）。**這個版本的簡體語言代碼是 `cn`，不是 FileDiver 說的 `sc`。**
- **`gen_mission_catalog.py`、`gen_game_text.py`：** 重新產生選單用的資料表。

## 遊戲內部（構建 25480438，RVA 都相對 game.dll）

### 戰況板
- **戰況板指標：** `[0x347CEE8]`。刷新函式 `RefreshBoard 0x12D58E0` 會把即時戰況複製到快照 `board+0x101438`，然後建立行動。
- **星球定義：** 每顆 `0x118` 位元組，`+0x18` 是 `settingsHash`，也就是星球內容／地形。
- **世界資料：** 每顆 `0x130` 位元組。faction `+0x46044`、enabled `+0x46050`、effects `+0x460e8`、effect 數量 `+0x46168`。
- **戰役列：** `+0x7004c`，每列 20 位元組，數量在 `+0x7284c`。
- **候選行動：** `+0xf7280`，每列 92 位元組，最多 110 列。目前進行中的行動在 `+0x78e88`。
- **行動列（92 B）欄位：** +0 index、+8 template id（建立前寫入）、+12 seed、+16 planet（u16）、+20 campaign、+0x1c 行動類型、+32 difficulty、+36 race、+0x34 有效、+56 template index、+60/+64 修飾、+68 修飾數量、+0x55.. 任務實例索引、+0x58 任務數。

### 行動範本表
- **位置：** `0x32FEF10`，共 25 列，每列 `0x490`。
- **欄位：** +0 id（= `operationSettingsId`）、+8 陣營、+0x14 類型、+0x24 名稱字串 id、+0x38 任務清單（32 組 `{u32 任務 id, f32 權重}`）、+0x138 修飾清單、+0x178 任務類型配額（每組 16 B，數量在 +0x208）。

### 任務表
- **位置：** `0x3773420`，共 162 列，每列 `0x380`。
- **欄位：** +0 id、+4 enabled、+8 陣營、+0xC/+0x10 難度範圍、+0x34 模式、+0x340 名稱字串 id、+0x368 可用區域清單的指標、+0x370 區域數量。
- **區域類型：** 1 一般、2 殖民地、3 城市、4 蟲巢、5 工廠。

### 建立行動卡
`BuildOperationRow 0x11E3250`：只挑範本和修飾，**不產生任務**。

### 產生任務
- **進入點：** `0x11E5670(container, ?, 行動列陣列, planet)`。戰況刷新（`0x12D599F`）或在戰術桌選到該星球（`0x12DB3B0`）時會呼叫。
- **流程：** 先把行動列的 `+0x58` 歸零，再依難度決定要產生幾個任務（`0x11E2B60`），每一個都依序做：
  1. **挑候選（`0x11E5100`）：** 先依範本 `+0x178` 的配額決定這一格要哪種任務類型，再用篩選函式 `0x11E6800` 從 +0x38 清單篩出所有符合條件的任務，候選以任務表的列索引存進第 10 個參數，回傳候選數量。這一步**不使用權重**。篩選條件包括陣營、難度、類型，以及地形有沒有任務要的區域。
  2. **挑地圖位置：** `0x11E6020`。
  3. **選定一個任務：** `0x11E6340`。候選只有 1 個時直接用它，不查權重。
  4. **建立任務實例：** 寫在 `container + i*0x4c`，數量在 `+0x61f8`，上限 330。

### 行動類型表
`0x32E98E0`，共 14 列，每列 `0xA8`。`+9` 是城市旗標，類型 8、9、10、12、13 有設。

## 目前的做法

1. **任務組：** hook `BuildOperationRow`。入口星球的候選行動列在建立前寫入範本 id（+8）和陣營（+36）；遊戲產生不出來時，就還原這一列，改用原版產生。每個難度都會套用。
2. **地形、陣營、詞條：**
   - 戰況刷新時（`refresh_detour`），**暫時**改寫即時戰況：`settingsHash`、enabled、faction、effects；需要的話補一筆戰役。刷新完立刻還原，只留在快照裡。
   - 行動進行中（`apply_active_world`，每一幀執行），對入口星球的行動常駐寫入同樣的設定，回到飛船、行動也結束後再還原。
3. **任務 1–3（test16 起）：** hook 挑候選的函式 `0x11E5100`。當行動列是入口星球的、而且第 `slot = +0x58` 格有指定任務時，把候選改成只有那一個、回傳 1。這樣會**跳過**類型配額和區域篩選。test21 起客機也會替換。
6. **同步碼（test21）：**
   - 編碼和解碼在 `menu.c` 的 `menu_preset_code()` / `menu_parse_code()`：開頭 `HF1-`，內容是 Crockford Base32，最後有 CRC-16。
   - 打包的內容：版本、planet（u16）、biome、faction、template（0xFF 代表原版）、任務 3 個（u32）、詞條數量和每個詞條（u32）。
   - 選單底部有「複製同步碼」「貼上同步碼並套用」；貼上後送出 `MENU_OPEN_TEMPORARILY`，交給 `arm()`。
   - 客機也能開 F11 和送出指令。`arm()` 在客機上（`joiner`）會跳過補戰役和戰況刷新，日誌記成 `Joiner follows the sync code`。
   - 變成客機時，不再停用 `armed`：`select_detour` 和 `apply_active_world` 的 `is_host()` 限制都拿掉了，客機照同一份設定改自己的遊戲。
   - `refresh_detour` 和 `build_row_detour` 仍然只在房主上執行。
   - **要注意：** 自己開放過設定的玩家，加入別人的房間後，設定也會繼續套用；要停止必須按「撤回開放」。
   - 同步碼的來回測試在 `tests/sync_code_test.c`，結果全部通過：能完整還原，大小寫不影響，改錯一個字元會被拒絕。
4. **行動結束後（test17）：** 不論完成、放棄或中途退出，星球都維持開放。回到飛船後會主動重建一次戰況。「臨時開放」持續到撤回開放或關閉遊戲。
5. **輸入處理（test13 起，和 F8 同一套）：**
   - 用 `SetWindowLongPtrW` 接到遊戲的視窗處理函式前面。
   - 選單開著時：鍵盤、滑鼠和 WM_INPUT 都交給選單（WM_INPUT 用 `DefWindowProc` 處理掉）；`GetRawInputBuffer` 全部清空並回傳 0；忽略遊戲呼叫的 `ClipCursor` / `SetCursorPos`；開啟時解除游標鎖定。
   - 選單關著時：除了 F11 之外完全不處理，遊戲自己會重新鎖游標。
   - 開選單時記下遊戲的原始輸入註冊，關閉時比對，被改過就照原樣註冊回去。`RegisterRawInputDevices` 有 hook，只用來記錄誰改了註冊。
   - 根據 test13 的日誌，關選單後遊戲有繼續收到滑鼠資料。

## 已知事實與限制

- **客機怎麼得到行動：** Operation_Create 只送出 `{operationSettingsId, seed, planetIndex, race, difficulty, missions[{index}]}`，客機用自己的範本表加上種子算出任務。所以**換整個任務組可以同步給客機**，但**自選任務 1–3 只有主機的遊戲會改**，沒裝模組的客機會不一致。選單裡有寫這個警告。
- **選單的變種組合（test20）：** 已改成和 HellForge 一致。
  - 孢裂變種只有 1244（拿掉 1386）；入侵艦隊只有 1413（拿掉 1414）；毒瘴改成即時戰況用的 1433＋1434（拿掉沒人用的 1432）。
  - 每個詞條都標上 [客機可見]、[客機部分可見] 或 [客機顯示?]；詞條頁的分類多了「客機可見（HellForge 有定義）」。
  - 毒瘴和腐蝕變種在遊戲 25480438 的文本裡找不到，這個版本可能沒有。
- **詞條定義：** 每台遊戲向自己的本機後端讀 `/WarSeason/GalacticWarEffects`。HellForge 0.4.0-beta 只內建 24 筆定義（`src/HellForge.LocalBackend/galactic-war-effects.json`），定義以外的 ID 會顯示成紫色問號。
  - 變種是一對效果：例如破地變種 `1303` 是 type 40，讓變種敵人進入可生成的名單；`1310` 是 type 71，負責星圖標記。
- **詞條能不能同步給客機，本模組還沒驗證。**
  - **已確認的只有房主端：** test19 的組隊紀錄裡，房主進任務時帶著 `1303`、`1310`（日誌 `mission start (host)`）。
  - **請求內容：** `Operation/Create` 和 `Mission/Start` 只帶行動和任務欄位，沒有詞條。
  - **原生封包：** HellForge 透過 Steam 轉送原生遊戲封包，但目前的紀錄判斷不了客機有沒有收到 F11 加上的 ID，也不知道客機有沒有真的刷出變種敵人。
  - **早期證據：** 2026-11-24 用參考模組 0.2.0 測試時，客機出現紫色問號，表示 ID 可能有送到。
  - **客機自己的分配：** HellForge 各機的本機後端也會自己分配變種（`OpenWar.cs`）。入房時只用 `war.clock` 對齊房主的戰況時間，種子仍取自各自的啟動設定；種子不同時，客機會載入哪些變種，HellForge 官方也列為待測。
- **火星：** 是 #263，內容 hash `1893896155`（planet_tutorial）。但目前的戰況板沒有 #263 的定義，所以會出現 `Unavailable entry planet=263`，**火星還不能當入口**。
- **沒有名稱的地形：** 像 `planet_313` 這種沒綁星球的地形，任務裡的星球名稱會顯示「保密地點」（推測和名稱來源有關）。
- **城市：** 由行動類型決定，也就是範本 2、3、10、11、17、18、22，不是由地形決定。
- **殖民地：** 要同時符合兩個條件：任務允許殖民地，而且地形本身有殖民地區域。
  - 只能出現在殖民地的任務（任務列）：蟲族 58、69、98–102；機器人 57；光能者 119、132、134、136、137。
  - 自選任務會跳過區域篩選；地形沒有殖民地時，遊戲會怎麼處理還不知道。
- **權重：** 範本 +0x38 清單的權重不影響候選；test15 改權重的做法完全無效，已經移除。
- **範本 19：** QUELL EXOSTORM 本身是停用的（enabled = 0）。
- **任務表裡沒被任何任務組用到的 50 個任務**（25 個還在啟用狀態），例如 DEPLOY DARK FLUID（#93）、幽暗遠征（#95–102）。暗流體的地圖和物件資源已經從現行版本刪掉，（舊版比對的分析不在這個倉庫）。

## 驗證狀態

**已驗證：**
- 範本覆寫在 D11 也有效。
- 地形換成 `settingsHash` 後，進任務時日誌顯示 `(this mod's)`。
- test13 之後，F11 不再卡視角。
- test16 的 `Mission N of planet … replaces …` 有記錄到。
- test19 組隊（房主角色）：中途退出後出現 `Operation on planet 1 ended; the planet stays open (this session), board rebuilt: yes`；任務 1–3 換成幽暗遠征的 #98、#101、#102；房主進任務時帶著 `1303`、`1310`。日誌在 `logs/sessions/812a642d12aa47eab525c7d63b40297d`。

**還沒驗證：**
- 兩台機器：客機的地形、變種、中途加入、8 人房（見 `TEST.md`）。
- 自選任務進入任務後能不能正常載入（跨陣營任務、幽暗遠征）。
- test18/19 的簡體轉換和官方名稱在遊戲裡的實際顯示。

## 可能的下一步

- 在任務清單標出每個任務可用的區域；加「強制殖民地」選項；在日誌記錄目前地形有哪些區域類型（要從執行中的星球資料讀）。
- 讓火星 #263 能當入口：補戰況板定義，或改由其他方式產生行動。
- DSS 飛鷹風暴：要注入 1209/1212/1216/1217 的定義，type 87/88 的數值目前不明。
- `README.md`（使用說明）和 `TEST.md`（驗收清單）已更新到 test19；之後每次改版要一起更新。

