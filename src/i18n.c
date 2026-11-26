#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#include "i18n.h"

/* Both columns of a format string take the same arguments in the same order. */
static const wchar_t *const texts[T_COUNT][LANG_COUNT] = {
    [T_WINDOW] = {L"HellForge · 房主選單", L"HellForge · Host menu"},
    [T_NAV_PLANET] = {L"①  星球", L"1   Planet"},
    [T_NAV_TERRAIN] = {L"②  地形", L"2   Terrain"},
    [T_NAV_OPERATION] = {L"③  行動", L"3   Operation"},
    [T_NAV_EFFECTS] = {L"④  詞條", L"4   Effects"},
    [T_SUM_OPERATION] = {L"陣營　%ls\n難度　%ls\n模板　%ls", L"Faction      %ls\nDifficulty   %ls\nTemplate     %ls"},
    [T_SUM_EFFECTS_NONE] = {L"詞條：無", L"Effects: none"},
    [T_SUM_EFFECTS_HEAD] = {L"詞條 %u 個，客機可見 %u 個", L"%u effect(s), %u seen by joiners"},
    [T_SUM_MORE] = {L"\n…另外 %u 個", L"\n…and %u more"},
    [T_TITLE] = {L"自訂星球與行動", L"CUSTOM PLANETS AND OPERATIONS"},
    [T_SHORTCUT] = {L"F11 / Esc 收起", L"F11 / Esc to close"},
    [T_LANG_BUTTON] = {L"語言", L"Language"},
    [T_TARGET_TITLE] = {L"星球與地形", L"Planet and terrain"},
    [T_LBL_PLANET_SEARCH] = {L"搜尋星球", L"Find planet"},
    [T_LBL_PLANET] = {L"入口星球", L"Entry planet"},
    [T_LBL_BIOME_SEARCH] = {L"搜尋地形", L"Find terrain"},
    [T_LBL_BIOME] = {L"地形來源", L"Terrain"},
    [T_LBL_LANDING] = {L"套用結果", L"Result"},
    [T_ACTION_TITLE] = {L"行動設定", L"Operation"},
    [T_LBL_FACTION] = {L"陣營", L"Faction"},
    [T_LBL_DIFFICULTY] = {L"難度", L"Difficulty"},
    [T_LBL_TEMPLATE] = {L"任務組模板", L"Operation template"},
    [T_LBL_SEED] = {L"種子（選填）", L"Seed (optional)"},
    [T_LBL_REGION] = {L"區域 0–7（選填）", L"Region 0–7 (optional)"},
    [T_EFFECT_TITLE] = {L"星球詞條", L"Planet effects"},
    [T_LBL_CATEGORY] = {L"類別", L"Category"},
    [T_LBL_SEARCH] = {L"搜尋", L"Search"},
    [T_BTN_CLOSE] = {L"撤回開放", L"Withdraw"},
    [T_BTN_SAVE] = {L"保存並開放", L"Save and open"},
    [T_BTN_TEMPORARY] = {L"臨時開放", L"Open (this session)"},

    [T_FACTION_NATIVE] = {L"自動陣營", L"Auto faction"},
    [T_FACTION_BUGS] = {L"蟲族", L"Terminids"},
    [T_FACTION_BOTS] = {L"機器人", L"Automatons"},
    [T_FACTION_SQUIDS] = {L"光能者", L"Illuminate"},
    [T_DIFFICULTY_TABLE] = {L"戰術桌自選", L"Pick on war table"},
    [T_TEMPLATE_GAME] = {L"原版生成", L"Game's own"},
    [T_CUE_PLANET] = {L"星球名稱或編號，例如 #1", L"Planet name or number, e.g. #1"},
    [T_CUE_TERRAIN] = {L"火星、針葉林、岩漿、寂域，或地形 hash", L"Mars, coniferous, magma, void, or a terrain hash"},
    [T_CUE_EFFECT] = {L"詞條名稱，或任意效果 ID", L"Effect name, or any effect ID"},
    [T_CUE_SEED] = {L"遊戲的種子", L"The game's seed"},
    [T_CUE_REGION] = {L"預設區域", L"Default region"},

    [T_PIN_MARS] = {L"★ 火星（太陽系）", L"★ Mars (Sol)"},
    [T_PIN_DESERT] = {L"★ 專屬沙漠", L"★ Unique desert"},
    [T_PIN_HIVE] = {L"★ 蟲巢", L"★ Hive world"},
    [T_PIN_VOID] = {L"★ 寂域森林", L"★ Void forest"},
    [T_PIN_FACTORY] = {L"★ 超大型工廠", L"★ Megafactory"},
    [T_PIN_MAGMA] = {L"★ 岩漿", L"★ Magma"},
    [T_PIN_SUPER_EARTH] = {L"★ 超級地球", L"★ Super Earth"},
    [T_PIN_NO_OPS] = {L"超級地球旁（不產生行動）", L"Beside Super Earth (no operations)"},
    [T_TER_MARS] = {L"★ 火星（訓練場 planet_tutorial）", L"★ Mars (training ground, planet_tutorial)"},
    [T_TER_MARS2] = {L"★ 火星訓練場 2", L"★ Mars training ground 2"},
    [T_TER_CONIFER] = {L"★ 針葉林（模板）", L"★ Coniferous (template)"},
    [T_TER_CONIFER_PLANET] = {L"★ 針葉林", L"★ Coniferous"},
    [T_TER_MAGMA] = {L"★ 岩漿（模板）", L"★ Magma (template)"},
    [T_TER_HIVE] = {L"★ 蟲巢（模板）", L"★ Hive world (template)"},
    [T_TER_VOID] = {L"★ 寂域森林", L"★ Void forest"},
    [T_TER_HIVE_PLANET] = {L"★ 蟲巢", L"★ Hive world"},
    [T_TER_FACTORY] = {L"★ 超大型工廠", L"★ Megafactory"},
    [T_TER_MAGMA_PLANET] = {L"★ 岩漿", L"★ Magma"},
    [T_TER_SUPER_EARTH] = {L"★ 超級地球", L"★ Super Earth"},
    [T_TER_UNREGISTERED] = {L"[未登記]  ", L"[unregistered]  "},
    [T_TER_NATIVE] = {L"使用星球原版地形", L"Planet's own terrain"},
    [T_TER_CUSTOM] = {L"自訂地形 hash  ·  %u", L"Custom terrain hash  ·  %u"},
    [T_ALIAS_VOID] = {L"寂域森林", L"Void forest"},
    [T_ALIAS_CONIFER] = {L"針葉林", L"Coniferous forest"},

    [T_LAND_PICK_PLANET] = {L"選擇星球", L"Choose a planet"},
    [T_LAND_PICK_PLANET_HINT] = {L"可輸入名稱或編號；★ 為特殊地形。", L"Type a name or number; ★ marks special terrain."},
    [T_LAND_NO_PLANET] = {L"沒有符合的星球", L"No matching planet"},
    [T_LAND_NO_PLANET_HINT] = {L"可輸入 0–273 的星球編號。", L"Type a planet number, 0–273."},
    [T_LAND_PICK_TERRAIN] = {L"選擇地形", L"Choose a terrain"},
    [T_LAND_NO_TERRAIN] = {L"沒有符合的地形", L"No matching terrain"},
    [T_LAND_PICK_TERRAIN_HINT] = {L"從結果選一套地形，或輸入地形 hash。", L"Pick a terrain from the results, or type a terrain hash."},
    [T_LAND_NO_TERRAIN_HINT] = {L"清空搜尋即使用星球原版地形。", L"Clear the search to keep the planet's own terrain."},
    [T_LAND_ENTRY] = {L"入口  %ls  ·  #%u", L"Entry  %ls  ·  #%u"},
    [T_LAND_OWN] = {L"原版地形  ·  %ls", L"Own terrain  ·  %ls"},
    [T_LAND_CUSTOM] = {L"地形改為自訂 hash %u（目錄外，實驗）", L"Terrain becomes custom hash %u (not in the catalogue, experimental)"},
    [T_LAND_DONOR] = {L"地形改為 %ls · %ls%ls", L"Terrain becomes %ls · %ls%ls"},
    [T_LAND_TEMPLATE_NOTE] = {L"（未登記模板）", L" (unregistered template)"},

    [T_TIER_ALL] = {L"客機可見", L"Joiners see it"},
    [T_TIER_PART] = {L"客機部分可見", L"Joiners see part"},
    [T_TIER_NONE] = {L"客機顯示?", L"Joiners see ?"},
    [T_F_ALL] = {L"全部星球詞條", L"All planet effects"},
    [T_F_BUGS] = {L"蟲族變種", L"Terminid variants"},
    [T_F_BOTS] = {L"機器人變種", L"Automaton variants"},
    [T_F_SQUIDS] = {L"光能者變種", L"Illuminate variants"},
    [T_F_SHARED] = {L"通用組合", L"Shared sets"},
    [T_F_VARIANTS] = {L"全部變種組合", L"All variant sets"},
    [T_F_SELECTED] = {L"已選效果 ID", L"Selected effect IDs"},
    [T_EFFECT_COUNT] = {L"已選 %u / %u 個 ID", L"%u / %u IDs selected"},
    [T_EFFECT_HINT] = {L"單擊或按空白鍵勾選，組合會選齊配套 ID。", L"Click or press Space to toggle; a set selects all of its IDs."},
    [T_EFFECT_EMPTY] = {L"清單是空的，可換類別或清空搜尋。", L"The list is empty; change the category or clear the search."},
    [T_EFFECT_CUSTOM_NAME] = {L"自訂效果 ID", L"Custom effect ID"},
    [T_EFFECT_CUSTOM_CATEGORY] = {L"目錄外", L"Not in the catalogue"},
    [T_EFFECT_UNNAMED] = {L"效果 #%u", L"Effect #%u"},
    [T_EFFECT_TOO_MANY] = {L"所選詞條超過 32 個不同的效果 ID，請先取消一些。", L"More than 32 different effect IDs are selected; untick some first."},

    [T_ERR_PLANET] = {L"請先從搜尋結果選一顆已登記的星球。", L"Choose a registered planet from the results first."},
    [T_ERR_DIFFICULTY] = {L"填了區域就要選難度。", L"Choose a difficulty when you fill in a region."},
    [T_ERR_REGION] = {L"區域填 0 至 7，留空使用預設。", L"Region is 0 to 7; leave it empty for the default."},
    [T_ERR_SEED] = {L"種子填十進位或 0x 十六進位數字，留空使用遊戲的種子。", L"The seed is a decimal or 0x hex number; leave it empty for the game's."},
    [T_ERR_FACTION] = {L"請選擇陣營。", L"Choose a faction."},
    [T_ERR_TERRAIN] = {L"請選擇地形來源（或清空地形搜尋使用原版地形）。", L"Choose a terrain (or clear the terrain search to keep the planet's own)."},
    [T_STATUS_READY] = {L"開放後，在原版戰術桌選擇任務。", L"Once open, pick missions on the war table."},
    [T_STATUS_WAIT] = {L"等待房主的艦船與戰術桌。", L"Waiting for the host's ship and war table."},
    [T_SUBMIT_UNAVAILABLE] = {L"目前無法送出。", L"Can't submit now."},
    [T_SUBMIT_PENDING] = {L"上一個請求還在處理。", L"The last request is still being handled."},
    [T_SUBMIT_SENT] = {L"已送出，等待戰術桌回應。", L"Sent; waiting for the war table."},
    [T_ERR_THREAD] = {L"無法啟動選單執行緒。", L"Couldn't start the menu thread."},
    [T_ERR_MODULE] = {L"無法取得選單模組。", L"Couldn't get the menu's module."},
    [T_ERR_REGISTER] = {L"無法註冊選單視窗。", L"Couldn't register the menu window."},
    [T_ERR_LOOP] = {L"選單訊息迴圈中止了。", L"The menu's message loop stopped."},
    [T_ERR_WINDOW] = {L"無法建立選單視窗。", L"Couldn't create the menu window."},

    [T_OP_MAX_EFFECTS] = {L"每顆星球最多 32 個不同的效果 ID。", L"A planet holds at most 32 different effect IDs."},
    [T_OP_TOO_MANY] = {L"所選詞條加上這顆星原有的效果超過 32 個不同 ID，請減少勾選。", L"The selected effects and the planet's own come to more than 32 IDs; untick some."},
    [T_OP_INVALID] = {L"原生戰役或星球欄位沒有通過檢查。", L"The native campaign or planet fields failed their checks."},
    [T_OP_REGION_FACTION] = {L"這個區域要用目前戰況的自動陣營。", L"This region needs the war's own faction."},
    [T_OP_OTHER_CAMPAIGN] = {L"這顆星已有其他類型或陣營的原生戰役。", L"The planet already has a native campaign of another kind or faction."},
    [T_OP_BUILD_MISMATCH] = {L"這個遊戲構建的戰術桌介面檢查失敗。", L"The war table does not match this game build."},
    [T_OP_HOOK_FAILED] = {L"房主選單無法連上戰術桌，請看模組日誌。", L"The host menu couldn't attach to the war table; see the mod log."},
    [T_OP_NOT_READY] = {L"戰術桌還沒準備好。", L"The war table isn't ready yet."},
    [T_OP_CHECK] = {L"請檢查難度與詞條數量。", L"Check the difficulty and the number of effects."},
    [T_OP_SELECTION] = {L"這個構建的原生行動選擇入口沒有通過檢查。", L"This build's native operation picker failed its checks."},
    [T_OP_PLANET] = {L"請選一顆已登記的星球。", L"Choose a registered planet."},
    [T_OP_NO_IDENTITY] = {L"戰況板上沒有這顆星的同構建定義（看日誌的 board 段落）。", L"The war board has no definition of this planet for this build (see the log's board lines)."},
    [T_OP_NO_REGION] = {L"目前戰況沒有這顆星的原版區域。", L"The war has no native region of this planet."},
    [T_OP_FACTION] = {L"這顆星沒有敵人陣營，請選一個。", L"This planet has no enemy faction; choose one."},
    [T_OP_SNAPSHOT] = {L"星球沒有進入戰術桌快照。", L"The planet did not reach the war table's snapshot."},
    [T_OP_SAVE] = {L"保存預設失敗。", L"Couldn't save the preset."},
    [T_OP_PLANET_N] = {L"星球 %u", L"Planet %u"},
    [T_OP_EXTRA_TERRAIN] = {L"，地形 %u", L", terrain %u"},
    [T_OP_EXTRA_TEMPLATE] = {L"，模板 %u", L", template %u"},
    [T_OP_OPENED] = {L"已開放 %ls（#%u）%ls，%u 個效果 ID。請在戰術桌選任務。", L"Opened %ls (#%u)%ls, %u effect ID(s). Pick missions on the war table."},
    [T_OP_NOT_CONFIRMED] = {L"戰術桌沒有確認行動選擇，請在戰術桌自己選。", L"The war table did not confirm the operation; pick it yourself."},
    [T_OP_SELECTED] = {L"已選中對應的原版行動組，請在戰術桌選任務。", L"The matching operation is selected; pick missions on the war table."},
    [T_OP_NO_CANDIDATE] = {L"戰術桌的候選裡沒有符合的原版行動組。", L"No matching operation among the war table's candidates."},
    [T_OP_JOINER] = {L"目前是客機：已套用的設定（同步碼）會繼續修改這台遊戲的任務、地形和詞條。", L"Running as a joiner: the applied settings (sync code) keep changing this game's missions, terrain and effects."},
    [T_OP_TEMP_ENDED] = {L"行動已結束，星球維持開放，可直接再開下一個行動。", L"The operation ended; the planet stays open for the next one."},
    [T_OP_QUEUED] = {L"已排入。請在戰術桌放棄或完成目前的行動，之後會自動套用。", L"Queued. Finish or abandon the active operation at the war table to apply this change."},
    [T_OP_WITHDRAWN] = {L"已撤回這顆星的開放。", L"The planet's opening is withdrawn."},
    [T_OP_WITHDRAWN_SAVE_FAILED] = {L"已撤回，但預設沒清掉。", L"Withdrawn, but the preset wasn't cleared."},
};

/* Template names as the Havoc table lists them (rows of game.dll+0x32FEF10); the log confirms what each creates. */
static const wchar_t *const templates[25][LANG_COUNT] = {
    {L"0 · 蟲族 · 保衛星球", L"0 · Terminid · Defend planet"},
    {L"1 · 蟲族 · 維和行動", L"1 · Terminid · Peacekeeping"},
    {L"2 · 蟲族 · 堅守城市", L"2 · Terminid · Hold city"},
    {L"3 · 蟲族 · 奪回城市", L"3 · Terminid · Reclaim city"},
    {L"4 · 蟲族 · 解放蟲巢世界", L"4 · Terminid · Liberate hive world"},
    {L"5 · 蟲族 · 守護 E-711", L"5 · Terminid · Safeguard Vital E-711"},
    {L"6 · 蟲族 · 啟動 TCS+", L"6 · Terminid · Activate TCS+"},
    {L"7 · 蟲族 · 試煉場行動", L"7 · Terminid · Proving ground"},
    {L"8 · 機器人 · 保衛星球", L"8 · Automaton · Defend planet"},
    {L"9 · 機器人 · 維和行動", L"9 · Automaton · Peacekeeping"},
    {L"10 · 機器人 · 堅守城市", L"10 · Automaton · Hold city"},
    {L"11 · 機器人 · 奪回城市", L"11 · Automaton · Reclaim city"},
    {L"12 · 機器人 · 突擊隊行動", L"12 · Automaton · Commando"},
    {L"13 · 機器人 · 解放超大型工廠", L"13 · Automaton · Liberate megafactory"},
    {L"14 · 機器人 · 解放生化斯坦", L"14 · Automaton · Liberate Cyberstan"},
    {L"15 · 光能者 · 擾亂光能者", L"15 · Illuminate · Disrupt Illuminate"},
    {L"16 · 光能者 · 保衛星球", L"16 · Illuminate · Defend planet"},
    {L"17 · 光能者 · 堅守城市", L"17 · Illuminate · Hold city"},
    {L"18 · 光能者 · 奪回城市", L"18 · Illuminate · Reclaim city"},
    {L"19 · 光能者 · 平息異界風暴", L"19 · Illuminate · Quell exostorm"},
    {L"20 · 光能者 · 阻止寂域", L"20 · Illuminate · Avert void"},
    {L"21 · 光能者 · 擾亂寂域", L"21 · Illuminate · Disrupt void"},
    {L"22 · 光能者 · 城市寂域", L"22 · Illuminate · City void"},
    {L"23 · 光能者 · 調查寂域星球", L"23 · Illuminate · Investigate void planet"},
    {L"24 · 光能者 · 調查寂域星球 II", L"24 · Illuminate · Investigate void planet II"},
};

static volatile LONG language = -1;

enum Language ui_language(void)
{
    LONG current = InterlockedCompareExchange(&language, -1, -1);
    if (current < 0) {
        const LANGID id = GetUserDefaultUILanguage();
        current = PRIMARYLANGID(id) != LANG_CHINESE ? LANG_EN :
                  SUBLANGID(id) == SUBLANG_CHINESE_SIMPLIFIED || SUBLANGID(id) == SUBLANG_CHINESE_SINGAPORE ? LANG_ZH_HANS :
                  LANG_ZH;
        InterlockedCompareExchange(&language, current, -1);
    }
    return current == LANG_EN ? LANG_EN : current == LANG_ZH_HANS ? LANG_ZH_HANS : LANG_ZH;
}

void set_ui_language(enum Language next)
{
    InterlockedExchange(&language, next == LANG_EN ? LANG_EN : next == LANG_ZH_HANS ? LANG_ZH_HANS : LANG_ZH);
}

/* Character mapping keeps Taiwan wording; these are the mainland words for the ones this menu uses. */
static const wchar_t *const mainland[][2] = {
    {L"选单", L"菜单"}, {L"搜寻", L"搜索"}, {L"设定", L"设置"}, {L"资料", L"数据"}, {L"档案", L"文件"},
    {L"载入", L"加载"}, {L"储存", L"保存"}, {L"支援", L"支持"}, {L"程式", L"程序"}, {L"讯息", L"信息"},
    {L"介面", L"界面"}, {L"滑鼠", L"鼠标"}, {L"视窗", L"窗口"}, {L"预设", L"预设"}, {L"日志", L"日志"},
    {L"游戏内", L"游戏内"}, {L"伺服器", L"服务器"}, {L"网路", L"网络"}, {L"品质", L"质量"},
};

void to_simplified(const wchar_t *text, wchar_t *out, size_t capacity)
{
    if (!capacity) return;
    out[0] = 0;
    if (!text) return;
    if (!LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, text, -1, out, (int)capacity, NULL, NULL, 0)) {
        wcsncpy(out, text, capacity - 1);
        out[capacity - 1] = 0;
        return;
    }
    for (size_t i = 0; i < sizeof mainland / sizeof mainland[0]; ++i) {
        const size_t from = wcslen(mainland[i][0]), to = wcslen(mainland[i][1]);
        for (wchar_t *at = wcsstr(out, mainland[i][0]); at; at = wcsstr(at + to, mainland[i][0])) {
            const size_t tail = wcslen(at + from);
            if ((size_t)(at - out) + to + tail + 1 > capacity) break;
            memmove(at + to, at + from, (tail + 1) * sizeof *at);
            memcpy(at, mainland[i][1], to * sizeof *at);
        }
    }
}

/* Converted texts, kept for the life of the process (the menu's strings are a few hundred). */
typedef struct Converted { const wchar_t *source; wchar_t *text; struct Converted *next; } Converted;
static Converted *converted;
static SRWLOCK converted_lock = SRWLOCK_INIT;

const wchar_t *simplified(const wchar_t *text)
{
    if (!text || !*text) return text;
    AcquireSRWLockShared(&converted_lock);
    for (const Converted *c = converted; c; c = c->next)
        if (c->source == text) { ReleaseSRWLockShared(&converted_lock); return c->text; }
    ReleaseSRWLockShared(&converted_lock);
    const size_t capacity = wcslen(text) * 2 + 16;
    Converted *entry = (Converted *)HeapAlloc(GetProcessHeap(), 0, sizeof *entry + capacity * sizeof(wchar_t));
    if (!entry) return text;
    entry->source = text;
    entry->text = (wchar_t *)(entry + 1);
    to_simplified(text, entry->text, capacity);
    AcquireSRWLockExclusive(&converted_lock);
    entry->next = converted;
    converted = entry;
    ReleaseSRWLockExclusive(&converted_lock);
    return entry->text;
}

const wchar_t *loc(const wchar_t *zh, const wchar_t *en)
{
    const enum Language current = ui_language();
    return current == LANG_EN ? en : current == LANG_ZH_HANS ? simplified(zh) : zh;
}

const wchar_t *tr(enum Text id)
{
    if ((unsigned)id >= T_COUNT) return L"";
    const wchar_t *zh = texts[id][LANG_ZH] ? texts[id][LANG_ZH] : L"";
    const wchar_t *en = texts[id][LANG_EN] ? texts[id][LANG_EN] : zh;
    return loc(zh, en);
}

const wchar_t *template_name(uint32_t index)
{
    return index < 25 ? loc(templates[index][LANG_ZH], templates[index][LANG_EN]) : L"";
}

static bool plain_ascii(const char *text, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)text[i] >= 0x80) return false;
    return true;
}

bool english_alias(const char *aliases, wchar_t *out, size_t capacity)
{
    /* Prefer the shortest English words ("PREDATOR STRAIN"), not snake_case keys, presence: rows or placeholders. */
    const char *best = NULL;
    size_t best_length = 0;
    for (const char *at = aliases; at && *at;) {
        const char *bar = strstr(at, " | ");
        const size_t length = bar ? (size_t)(bar - at) : strlen(at);
        bool usable = length > 2 && plain_ascii(at, length) && memchr(at, ' ', length) &&
                      strncmp(at, "presence:", 9) != 0 && strncmp(at, "Unresolved effect", 17) != 0;
        if (usable && (!best || length < best_length)) {
            best = at;
            best_length = length;
        }
        at = bar ? bar + 3 : NULL;
    }
    if (!best || capacity == 0) return false;
    size_t n = best_length < capacity - 1 ? best_length : capacity - 1;
    for (size_t i = 0; i < n; ++i) out[i] = (wchar_t)(unsigned char)best[i];
    out[n] = 0;
    return true;
}

const wchar_t *english_category(const char *category)
{
    static const struct { const char *zh; const wchar_t *en; } names[] = {
        {"临时装备", L"Temporary equipment"}, {"临时配发战备", L"Temporary stratagems"},
        {"事件作战", L"Event operations"}, {"作战增减益", L"Operation modifiers"},
        {"实验性战备变体", L"Experimental stratagem variants"}, {"弹药与补给", L"Ammunition and supply"},
        {"情报与侦察", L"Intel and recon"}, {"敌情与变种", L"Enemies and variants"},
        {"星球环境与通路", L"Environment and routes"}, {"未解效果", L"Unresolved effects"},
        {"空间站与战术行动", L"DSS and tactical actions"}, {"设施与星球标记", L"Facilities and markers"},
        {"配套星图词条", L"Map tags"}, {"通用组合", L"Shared sets"}, {"虫族变种", L"Terminid variants"},
        {"机器人变种", L"Automaton variants"}, {"光能族变种", L"Illuminate variants"},
    };
    for (size_t i = 0; category && i < sizeof names / sizeof names[0]; ++i)
        if (!strcmp(names[i].zh, category)) return names[i].en;
    return NULL;
}
