#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "imgui.h"

extern "C" {
#include "menu.h"
#include "effects.h"
#include "i18n.h"
#include "game_text.h"
}

#include <algorithm>
#include <map>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string utf8(const wchar_t *s)
{
    if (!s || !*s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (!n) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
    out.pop_back();
    return out;
}

std::string traditional(const std::string &s)
{
    if (std::all_of(s.begin(), s.end(), [](unsigned char c) { return c < 128; }))
        return s;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
    if (!n) return s;
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, &wide[0], n);
    n = LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, wide.c_str(), -1, nullptr, 0, nullptr, nullptr, 0);
    if (!n) return s;
    std::wstring mapped(n, L'\0');
    if (!LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, wide.c_str(), -1, &mapped[0], n, nullptr, nullptr, 0))
        return s;
    return utf8(mapped.c_str());
}

// Catalogue names are Simplified; shown in the menu's Chinese.
std::string chinese(const std::string &s)
{
    if (ui_language() != LANG_ZH_HANS) return traditional(s);
    if (std::all_of(s.begin(), s.end(), [](unsigned char c) { return c < 128; })) return s;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
    if (!n) return s;
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, &wide[0], n);
    std::wstring out(wide.size() * 2 + 16, L'\0');
    to_simplified(wide.c_str(), &out[0], out.size());
    return utf8(out.c_str());
}

const char *t(Text id)
{
    static std::string strings[LANG_COUNT][T_COUNT];
    std::string &s = strings[ui_language()][id];
    if (s.empty()) s = utf8(tr(id));
    return s.c_str();
}

const char *local(const char *zh, const char *en)
{
    const Language current = ui_language();
    if (current != LANG_ZH_HANS) return current == LANG_EN ? en : zh;
    static std::map<const char *, std::string> cache; // menu thread only
    auto found = cache.find(zh);
    if (found == cache.end()) found = cache.emplace(zh, chinese(zh)).first;
    return found->second.c_str();
}

std::string search_key(std::string s)
{
    s = traditional(s);
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Only a complete unsigned ID is a numeric search. An overflow must never select a different row.
bool read_id(const std::string &s, uint32_t &id)
{
    size_t i = !s.empty() && s[0] == '#' ? 1 : 0;
    unsigned base = 10;
    if (s.compare(i, 2, "0x") == 0) { base = 16; i += 2; }
    if (i == s.size()) return false;
    uint32_t value = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
        else return false;
        if (digit >= base || value > (UINT32_MAX - digit) / base) return false;
        value = value * base + digit;
    }
    id = value;
    return true;
}

struct Query {
    std::string key;
    uint32_t id = 0;
    bool numeric = false;

    explicit Query(const char *s) : key(search_key(s)) {
        const size_t start = key.find_first_not_of(" \t");
        key = start == key.npos ? "" : key.substr(start, key.find_last_not_of(" \t") - start + 1);
        numeric = read_id(key, id);
    }
    // A number finds its exact ID and also any name containing it ("313" finds planet_313).
    bool exact(uint32_t value) const { return numeric && id == value; }
    bool matches(uint32_t value, const std::string &haystack) const {
        return exact(value) || haystack.find(key) != key.npos;
    }
};

struct Choice {
    uint32_t value;
    std::string label, key;
    int category;
};

Choice choice(uint32_t value, const std::string &label, const std::string &aliases = {})
{
    return {value, label, search_key(label + " " + aliases), 0};
}

const Choice *find_choice(const std::vector<Choice> &rows, uint32_t value)
{
    for (const Choice &row : rows)
        if (row.value == value) return &row;
    return nullptr;
}

const char *terrain_aliases(const BiomeEntry &b)
{
    if (!std::strcmp(b.setup, "forest_rift_active")) return "寂域森林 西斯 void forest";
    if (!std::strcmp(b.setup, "forest_coniferous_base")) return "針葉林 coniferous confierous conifer forest";
    if (!std::strncmp(b.setup, "tutorial", 8)) return "火星 訓練場 Mars training ground";
    if (std::strstr(b.setup, "magma")) return "岩漿 熔岩 magma";
    if (std::strstr(b.setup, "hiveworld")) return "蟲巢 hive world";
    if (std::strstr(b.setup, "cyberstan")) return "超大型工廠 megafactory";
    return "";
}

uint32_t template_faction(uint32_t row)
{
    if (row >= HOST_OPERATIONS_TEMPLATES) return 0;
    return row < 8 ? 2u : row < 15 ? 3u : 4u;
}

// The game's own text in the menu's language, or nullptr where the game has none.
const char *game(const GameText &g)
{
    const char *s = g.text[ui_language()];
    return s && *s ? s : nullptr;
}

std::string game_all(const GameText &g)
{
    return std::string(g.text[0]) + " " + g.text[1] + " " + g.text[2];
}

const GameText *planet_game_text(const char *english)
{
    for (size_t i = 0; i < planet_text_count; ++i)
        if (!std::strcmp(planet_text[i].english, english)) return &planet_text[i].name;
    return nullptr;
}

const GameText *effect_game_text(uint32_t id)
{
    for (size_t i = 0; i < effect_text_count; ++i)
        if (effect_text[i].id == id) return &effect_text[i].name;
    return nullptr;
}

const char *faction_name(uint32_t faction)
{
    if (faction >= 2 && faction <= 4)
        if (const char *s = game(faction_text[faction])) return s;
    switch (faction) {
    case 0: return t(T_FACTION_NATIVE);
    case 2: return t(T_FACTION_BUGS);
    case 3: return t(T_FACTION_BOTS);
    case 4: return t(T_FACTION_SQUIDS);
    default: return t(T_ERR_FACTION);
    }
}

enum { All = -1, Selected = -2, Groups = -3, Known = -4 };

constexpr ImU32 amber = IM_COL32(234, 183, 88, 255);

struct EffectRow {
    uint32_t ids[3];
    int category;
    bool group;
    bool known; // every ID has a definition in HellForge's built-in effect table, so joiners can use it
    std::string label, key;
};

const char *mission_mode(uint32_t mode)
{
    switch (mode) {
    case 1: return local("一般", "Normal");
    case 2: return local("Horde", "Horde");
    case 3: return local("防禦/短任務", "Defense");
    case 4: return local("Opportunity", "Opportunity");
    case 5: return local("教學", "Tutorial");
    case 6: return local("閃擊", "Blitz");
    case 7: return local("幽暗遠征", "Gloom expedition");
    case 8: return local("突擊隊", "Commando");
    default: return "?";
    }
}

struct MissionRow {
    const MissionEntry *entry;
    std::string label, key;
};

struct Catalogue {
    Language language = LANG_COUNT;
    std::vector<MissionRow> missions;
    std::vector<Choice> planet, terrain, task;
    std::vector<EffectRow> effects;
    std::vector<std::string> categories;

    void add_effect(const uint32_t ids[3], bool group, const char *name, const char *aliases, const char *category) {
        const std::string zh = chinese(category);
        const std::string en = utf8(english_category(category));
        const std::string &cat = language == LANG_EN && !en.empty() ? en : zh;
        auto pos = std::find(categories.begin(), categories.end(), cat);
        int index = static_cast<int>(pos - categories.begin());
        if (pos == categories.end()) categories.push_back(cat);

        wchar_t translated[256];
        std::string label = chinese(name);
        const GameText *official = effect_game_text(ids[0]);
        if (official && game(*official)) label = game(*official);
        else if (language == LANG_EN) {
            if (english_alias(aliases, translated, sizeof translated / sizeof *translated)) label = utf8(translated);
            else {
                // Some named effects have only a single English word or a native lookup key.
                std::string first(aliases);
                first = first.substr(0, first.find(" | "));
                if (!first.empty() && first.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != first.npos
                    && std::all_of(first.begin(), first.end(), [](unsigned char c) { return c < 128; }))
                    label = first;
                else label = "Effect #" + std::to_string(ids[0]);
            }
        }
        label += "  ·  #" + std::to_string(ids[0]);
        for (size_t i = 1; i < 3 && ids[i]; ++i) label += " + " + std::to_string(ids[i]);
        label += "  ·  " + cat;
        unsigned count = 0, known = 0;
        for (size_t i = 0; i < 3 && ids[i]; ++i, ++count) known += joiner_knows_effect(ids[i]);
        label += std::string("  ·  [") + t(known == count ? T_TIER_ALL : known ? T_TIER_PART : T_TIER_NONE) + "]";
        effects.push_back({{ids[0], ids[1], ids[2]}, index, group, known == count, label,
                           search_key(label + " " + name + " " + aliases + " " + zh + " " + en +
                                      (official ? " " + game_all(*official) : std::string()))});
    }

    void refresh() {
        if (language == ui_language()) return;
        language = ui_language();
        planet.clear(); terrain.clear(); task.clear(); effects.clear(); categories.clear(); missions.clear();
        for (size_t i = 0; i < mission_catalog_count; ++i) {
            const MissionEntry &m = mission_catalog[i];
            const char *official = game(mission_text[m.row]);
            std::string label = official ? official : *m.name ? m.name : local("（無名稱）", "(unnamed)");
            label += "  ·  " + std::string(faction_name(m.faction)) + "  ·  " + mission_mode(m.mode) + "  ·  D" +
                     std::to_string(m.min_difficulty) + "–" + std::to_string(m.max_difficulty) + "  ·  #" +
                     std::to_string(m.row) + "  ·  " + std::to_string(m.id);
            if (!m.used_by) label += local("  ·  [隱藏]", "  ·  [hidden]");
            if (!m.enabled) label += local("  ·  [停用]", "  ·  [disabled]");
            missions.push_back({&m, label, search_key(label + " " + mission_mode(m.mode) + " " + m.name + " " +
                                                      game_all(mission_text[m.row]))});
        }
        terrain.push_back(choice(0, t(T_TER_NATIVE)));
        for (size_t i = 0; i < biome_count; ++i) {
            const BiomeEntry &b = biomes[i];
            const GameText *official = planet_game_text(b.name);
            std::string label = std::string(official && game(*official) ? game(*official) : b.name) + "  ·  " + b.setup;
            if (!std::strcmp(b.setup, "forest_rift_active")) label += "  ·  " + std::string(t(T_ALIAS_VOID));
            if (!std::strcmp(b.setup, "forest_coniferous_base")) label += "  ·  " + std::string(t(T_ALIAS_CONIFER));
            label += "  ·  " + std::to_string(b.settings_hash);
            terrain.push_back(choice(b.settings_hash, label, std::string(b.region) + " " + terrain_aliases(b) + " " + b.name +
                                     (official ? " " + game_all(*official) : std::string())));
            terrain.back().category = b.original_planet == HOST_OPERATIONS_NO_INDEX ? 2 : 1;
        }
        for (size_t i = 0; i < planet_count; ++i) {
            const PlanetEntry &p = planets[i];
            const GameText *official = planet_game_text(p.name);
            std::string label = "#" + std::to_string(p.index) + "  " + (official && game(*official) ? game(*official) : p.name);
            std::string aliases = std::string(p.region) + " " + p.name + (official ? " " + game_all(*official) : std::string());
            if (!std::strcmp(p.name, "MARS")) label += local("（測試入口）", " (test entry)");
            if (!std::strcmp(p.name, "SUPER EARTH")) aliases += " 超級地球";
            const Choice *own = find_choice(terrain, p.settings_hash);
            if (own) aliases += " " + own->key;
            planet.push_back(choice(p.index, label, aliases));
            planet.back().category = static_cast<int>(p.original_faction);
        }
        task.push_back(choice(HOST_OPERATIONS_NO_TEMPLATE, t(T_TEMPLATE_GAME)));
        for (uint32_t i = 0; i < HOST_OPERATIONS_TEMPLATES; ++i) {
            const char *official = game(template_text[i]);
            std::string label = official ? std::to_string(i) + " · " + faction_name(template_faction(i)) + " · " + official :
                                           utf8(template_name(i));
            if (i == 7) label += local("（請在戰術桌選 D1）", " (select D1 on the war table)");
            task.push_back(choice(i, label, utf8(template_name(i)) + " " + game_all(template_text[i])));
            task.back().category = static_cast<int>(template_faction(i));
        }
        for (size_t i = 0; i < variant_count; ++i) {
            const VariantEntry &v = variants[i];
            const char *cat = v.faction == 2 ? "虫族变种" : v.faction == 3 ? "机器人变种" :
                              v.faction == 4 ? "光能族变种" : "通用组合";
            add_effect(v.ids, true, v.name, v.aliases, cat);
        }
        for (size_t i = 0; i < planet_effect_count; ++i) {
            const PlanetEffectEntry &e = planet_effects[i];
            const uint32_t ids[3] = {e.id, 0, 0};
            add_effect(ids, false, e.name, e.aliases, e.category);
        }
    }
};

struct ListFilter {
    char search[128]{};
    int category = All;
    bool needs_choice = false;
    std::vector<const Choice *> visible;
};

struct Editor {
    OperationPreset draft{};
    Catalogue catalog;
    ListFilter planets, terrains, tasks;
    char mission_search[128]{};
    int mission_category = All;
    int mission_slot = 0;
    std::vector<const MissionRow *> visible_missions;
    char effect_search[128]{};
    std::vector<const EffectRow *> visible_effects;
    int category = All;
    Text feedback = T_COUNT;
    double feedback_until = 0;
    std::string code_message;   // result of the last copy or paste of a sync code
    double code_message_until = 0;

    Editor() {
        draft.planet = draft.region = HOST_OPERATIONS_NO_INDEX;
        draft.template_index = HOST_OPERATIONS_NO_TEMPLATE;
        menu_load_saved(&draft);
        // Older presets carried a candidate operation, difficulty, region and seed. The war table owns all four.
        draft.region = HOST_OPERATIONS_NO_INDEX;
        draft.operation_template = draft.difficulty = draft.seed = 0;
        if (draft.faction == 0) draft.faction = template_faction(draft.template_index);
    }
    void report(Text message) {
        feedback = message;
        feedback_until = ImGui::GetTime() + 4.0;
    }
};

std::string preview(const std::vector<Choice> &rows, uint32_t value, const char *fallback)
{
    const Choice *row = find_choice(rows, value);
    if (row) return row->label;
    if (value == HOST_OPERATIONS_NO_INDEX) return fallback;
    return std::string(fallback) + "  #" + std::to_string(value);
}

enum class ListKind { Planet, Terrain, Task };

// Leave room for the summary, status and command row. At short heights the parent window scrolls.
float list_height(float extra_lines)
{
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float footer = (7.1f + extra_lines) * line + 2.0f * ImGui::GetFontSize();
    return std::max(4.0f * line, ImGui::GetContentRegionAvail().y - footer);
}

const char *category_name(ListKind kind, int id)
{
    if (id == All) return local("全部", "All");
    if (kind == ListKind::Terrain)
        return id == 1 ? local("星球地形", "Planet terrain") : local("獨立地形資產", "Terrain assets");
    if (id == 1) return local("超級地球", "Super Earth");
    return faction_name(static_cast<uint32_t>(id));
}

bool choice_list(ListKind kind, ListFilter &filter, const std::vector<Choice> &rows, uint32_t &value)
{
    bool changed = false;
    const int first = kind == ListKind::Task ? 2 : 1;
    const int last = kind == ListKind::Terrain ? 2 : 4;
    ImGui::TextUnformatted(kind == ListKind::Planet ? local("目錄陣營", "Catalogue faction") : t(T_LBL_CATEGORY));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##category", category_name(kind, filter.category))) {
        if (ImGui::Selectable(category_name(kind, All), filter.category == All)) { filter.category = All; changed = true; }
        for (int id = first; id <= last; ++id) {
            if (ImGui::Selectable(category_name(kind, id), filter.category == id)) { filter.category = id; changed = true; }
        }
        ImGui::EndCombo();
    }
    const Text hint = kind == ListKind::Planet ? T_CUE_PLANET : kind == ListKind::Terrain ? T_CUE_TERRAIN : T_LBL_SEARCH;
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool enter = ImGui::InputTextWithHint("##filter", t(hint), filter.search, sizeof filter.search,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    changed |= ImGui::IsItemEdited();
    const Query query(filter.search);
    const bool custom = kind == ListKind::Terrain && filter.category == All && query.numeric && !find_choice(rows, query.id);
    bool picked = false;
    auto &visible = filter.visible;
    visible.clear();
    bool current_visible = custom && value == query.id;
    for (const Choice &row : rows) {
        if ((filter.category == All || filter.category == row.category) && query.matches(row.value, row.key)) {
            visible.push_back(&row);
            current_visible |= row.value == value;
        }
    }
    std::stable_partition(visible.begin(), visible.end(), [&](const Choice *row) { return query.exact(row->value); });
    if (changed) filter.needs_choice = (!query.key.empty() || filter.category != All) && !current_visible;
    if (changed) ImGui::SetNextWindowScroll(ImVec2(0, 0));
    if (ImGui::BeginChild("results", ImVec2(0, list_height(kind == ListKind::Task ? 4.5f : 0.0f)), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Choice &row = *visible[i];
                if (ImGui::Selectable(row.label.c_str(), row.value == value)) {
                    value = row.value;
                    picked = true;
                }
            }
        }
        if (custom) {
            const std::string label = local("使用地形 hash ", "Use terrain hash ") + std::to_string(query.id);
            if (ImGui::Selectable(label.c_str(), value == query.id)) { value = query.id; picked = true; }
        } else if (visible.empty()) {
            const char *none = kind == ListKind::Planet ? t(T_LAND_NO_PLANET) : kind == ListKind::Terrain ?
                               t(T_LAND_NO_TERRAIN) : local("沒有符合的模板", "No matching template");
            ImGui::TextWrapped("%s", none);
        }
    }
    ImGui::EndChild();
    if (enter && !visible.empty() && query.exact(visible.front()->value)) { value = visible.front()->value; picked = true; }
    else if (enter && custom) { value = query.id; picked = true; }
    else if (enter && visible.size() == 1) { value = visible.front()->value; picked = true; }
    if (picked) filter.needs_choice = false;
    return picked;
}

bool has_effect(const OperationPreset &p, uint32_t id)
{
    return effect_position(p.effects, p.effect_count, id) < p.effect_count;
}

// Apply a whole group to a copy so a full list cannot be left with half a variant added.
bool toggle_effect(OperationPreset &p, const uint32_t ids[3], bool remove)
{
    OperationPreset next = p;
    for (size_t i = 0; i < 3 && ids[i]; ++i) {
        if (remove) {
            auto end = std::remove(next.effects, next.effects + next.effect_count, ids[i]);
            next.effect_count = static_cast<uint32_t>(end - next.effects);
        } else if (!append_effect_id(next.effects, &next.effect_count, ids[i])) return false;
    }
    p = next;
    return true;
}

void effect_checkbox(Editor &ui, const uint32_t ids[3], const std::string &label)
{
    unsigned selected = 0, count = 0;
    while (count < 3 && ids[count]) {
        selected += has_effect(ui.draft, ids[count]);
        ++count;
    }
    const bool full = selected == count;
    bool checked = full;
    std::string widget;
    if (selected && !full)
        widget = "(" + std::to_string(selected) + "/" + std::to_string(count) + ") ";
    widget += label + "###toggle";
    if (ImGui::Checkbox(widget.c_str(), &checked)) {
        if (!toggle_effect(ui.draft, ids, full)) ui.report(T_EFFECT_TOO_MANY);
        else ui.feedback = T_COUNT;
    }
}

void draw_effects(Editor &ui)
{
    const auto &catalog = ui.catalog;
    const auto &cats = catalog.categories;
    const char *known_name = local("客機可見（HellForge 有定義）", "Joiners see it (defined by HellForge)");
    const char *current = ui.category == All ? t(T_F_ALL) : ui.category == Selected ? t(T_F_SELECTED) :
                          ui.category == Groups ? t(T_F_VARIANTS) : ui.category == Known ? known_name :
                          cats[ui.category].c_str();
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool reset_scroll = false;
    if (ImGui::BeginCombo("##category", current)) {
        for (int i = Known; i < static_cast<int>(cats.size()); ++i) {
            const char *name = i == All ? t(T_F_ALL) : i == Selected ? t(T_F_SELECTED) :
                               i == Groups ? t(T_F_VARIANTS) : i == Known ? known_name : cats[i].c_str();
            if (ImGui::Selectable(name, i == ui.category)) { ui.category = i; reset_scroll = true; }
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    reset_scroll |= ImGui::InputTextWithHint("##effectSearch", t(T_CUE_EFFECT), ui.effect_search, sizeof ui.effect_search);
    const Query q(ui.effect_search);
    if (ImGui::BeginChild("effectRows", ImVec2(0, list_height(0)), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        if (reset_scroll) ImGui::SetScrollY(0);
        auto &visible = ui.visible_effects;
        visible.clear();
        for (const EffectRow &row : catalog.effects) {
            if (ui.category >= 0 && ui.category != row.category) continue;
            if (ui.category == Groups && !row.group) continue;
            if (ui.category == Known && !row.known) continue;
            if (ui.category == Selected && (row.group || !has_effect(ui.draft, row.ids[0]))) continue;
            if (q.numeric ? !q.id || std::find(row.ids, row.ids + 3, q.id) == row.ids + 3 :
                            row.key.find(q.key) == std::string::npos) continue;
            visible.push_back(&row);
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const EffectRow &row = *visible[i];
                ImGui::PushID(&row);
                effect_checkbox(ui, row.ids, row.label);
                ImGui::PopID();
            }
        }
        bool any = !visible.empty();
        if (ui.category == All || ui.category == Selected) {
            // Snapshot IDs before drawing. Removing one checkbox must not skip or relabel its neighbour.
            const OperationPreset selected = ui.draft;
            auto custom_row = [&](uint32_t id) {
                const uint32_t ids[3] = {id, 0, 0};
                const std::string label = std::string(t(T_EFFECT_CUSTOM_NAME)) + "  #" + std::to_string(id);
                if (!q.matches(id, search_key(label))) return;
                ImGui::PushID(static_cast<int>(id));
                effect_checkbox(ui, ids, label);
                ImGui::PopID();
                any = true;
            };
            for (uint32_t i = 0; i < selected.effect_count; ++i)
                if (!known_effect_id(selected.effects[i])) custom_row(selected.effects[i]);
            if (ui.category == All && q.numeric && q.id && !known_effect_id(q.id) && !has_effect(selected, q.id))
                custom_row(q.id);
        }
        if (!any) ImGui::TextWrapped("%s", t(T_EFFECT_EMPTY));
    }
    ImGui::EndChild();
}

std::string terrain_name(const Editor &ui)
{
    const Choice *row = find_choice(ui.catalog.terrain, ui.draft.biome);
    return row ? row->label : local("自訂地形 hash ", "Custom terrain hash ") + std::to_string(ui.draft.biome);
}

void draw_operation(Editor &ui)
{
    auto &p = ui.draft;
    if (choice_list(ListKind::Task, ui.tasks, ui.catalog.task, p.template_index)) {
        const uint32_t faction = template_faction(p.template_index);
        if (faction) p.faction = faction;
    }
    const uint32_t required = template_faction(p.template_index);
    ImGui::TextUnformatted(t(T_LBL_FACTION));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##faction", faction_name(p.faction))) {
        for (uint32_t faction : {0u, 2u, 3u, 4u}) {
            ImGui::BeginDisabled(required && faction != required);
            if (ImGui::Selectable(faction_name(faction), p.faction == faction)) p.faction = faction;
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    if (required) ImGui::TextDisabled("%s", local("陣營由任務組模板決定。", "The operation template sets the faction."));
    ImGui::TextDisabled("%s", local("難度由戰術桌選擇，模組不更動難度。", "The war table sets the difficulty; the mod leaves it alone."));
}

const MissionRow *find_mission(const Catalogue &catalog, uint32_t id)
{
    for (const MissionRow &row : catalog.missions)
        if (row.entry->id == id) return &row;
    return nullptr;
}

std::string mission_text(const Editor &ui, uint32_t id)
{
    if (!id) return local("任務組原本的抽選", "The template's own draw");
    const MissionRow *row = find_mission(ui.catalog, id);
    return row ? row->label : local("未知任務 ", "Unknown mission ") + std::to_string(id);
}

// Missions 1-3 of the entry planet's operations, picked from every row of the game's mission table.
void draw_missions(Editor &ui)
{
    auto &p = ui.draft;
    ImGui::PushStyleColor(ImGuiCol_Text, amber);
    ImGui::TextWrapped("%s", local("只有主機的遊戲會換任務。沒裝本模組的客機仍按任務組和種子計算任務，兩邊會不一致；"
                                   "請單人遊玩，或讓所有玩家都裝同一版模組。",
                                   "Only the host's game changes the missions. Joiners without this mod still derive them from the "
                                   "template and the seed, so the two disagree: play solo, or have everyone install this mod."));
    ImGui::PopStyleColor();
    for (int i = 0; i < static_cast<int>(HOST_OPERATIONS_MISSION_SLOTS); ++i) {
        ImGui::PushID(i);
        const std::string slot = local("任務 ", "Mission ") + std::to_string(i + 1);
        ImGui::RadioButton(slot.c_str(), &ui.mission_slot, i);
        ImGui::SameLine();
        ImGui::BeginDisabled(!p.missions[i]);
        if (ImGui::SmallButton(local("清除", "Clear"))) p.missions[i] = 0;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", mission_text(ui, p.missions[i]).c_str());
        ImGui::PopID();
    }
    ImGui::TextDisabled("%s", local("實際有幾個任務由難度決定，依序用任務 1、2、3；陣營和任務組不同的任務可能產生不出來。",
                                    "The difficulty sets how many missions there are, taken as 1, 2, 3 in order; a mission of "
                                    "another faction than the template may fail to generate."));

    const int hidden = 5, chosen = 6;
    auto category = [&](int id) -> const char * {
        if (id == All) return local("全部", "All");
        if (id == hidden) return local("沒有任務組使用（隱藏）", "Used by no template (hidden)");
        if (id == chosen) return local("已選", "Chosen");
        return faction_name(static_cast<uint32_t>(id));
    };
    bool reset_scroll = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##missionCategory", category(ui.mission_category))) {
        for (int id : {static_cast<int>(All), 2, 3, 4, hidden, chosen})
            if (ImGui::Selectable(category(id), ui.mission_category == id)) { ui.mission_category = id; reset_scroll = true; }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    reset_scroll |= ImGui::InputTextWithHint("##missionSearch", local("搜尋任務名稱、任務 ID 或任務列", "Search name, mission ID or row"),
                                             ui.mission_search, sizeof ui.mission_search);
    const Query q(ui.mission_search);
    auto &visible = ui.visible_missions;
    visible.clear();
    for (const MissionRow &row : ui.catalog.missions) {
        const MissionEntry &m = *row.entry;
        const bool picked = std::find(p.missions, p.missions + HOST_OPERATIONS_MISSION_SLOTS, m.id) !=
                            p.missions + HOST_OPERATIONS_MISSION_SLOTS;
        if (ui.mission_category == hidden ? m.used_by != 0 : ui.mission_category == chosen ? !picked :
            ui.mission_category >= 2 && static_cast<uint32_t>(ui.mission_category) != m.faction) continue;
        if (!q.exact(m.id) && !(q.numeric && q.id == m.row) && row.key.find(q.key) == std::string::npos) continue;
        visible.push_back(&row);
    }
    if (ImGui::BeginChild("missionRows", ImVec2(0, list_height(0)), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        if (reset_scroll) ImGui::SetScrollY(0);
        uint32_t &slot = p.missions[ui.mission_slot];
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const MissionRow &row = *visible[i];
                ImGui::PushID(&row);
                if (ImGui::Selectable(row.label.c_str(), slot == row.entry->id)) {
                    for (uint32_t &other : p.missions)
                        if (other == row.entry->id) other = 0; /* one mission, one slot */
                    slot = row.entry->id;
                    if (ui.mission_slot + 1 < static_cast<int>(HOST_OPERATIONS_MISSION_SLOTS)) ++ui.mission_slot;
                }
                ImGui::PopID();
            }
        }
        if (visible.empty()) ImGui::TextWrapped("%s", local("沒有符合的任務", "No matching mission"));
    }
    ImGui::EndChild();
}

std::string problem(const Editor &ui)
{
    const OperationPreset &p = ui.draft;
    if (!find_choice(ui.catalog.planet, p.planet)) {
        if (p.planet == HOST_OPERATIONS_NO_INDEX) return t(T_ERR_PLANET);
        return "#" + std::to_string(p.planet) + "  ·  " + t(T_ERR_PLANET);
    }
    if (ui.planets.needs_choice) return t(T_ERR_PLANET);
    if (ui.terrains.needs_choice) return local("請從地形結果中選取一列，或清除篩選。", "Select a terrain result or clear its filters.");
    if (ui.tasks.needs_choice) return local("請從模板結果中選取一列，或清除篩選。", "Select a template result or clear its filters.");
    if (p.template_index != HOST_OPERATIONS_NO_TEMPLATE && p.template_index >= HOST_OPERATIONS_TEMPLATES)
        return local("請選擇 0–24 號模板或原版生成。", "Choose template 0–24 or the game's own.");
    const uint32_t required = template_faction(p.template_index);
    if (required && required != p.faction)
        return std::string(local("此模板需要的陣營為 ", "This template requires ")) + faction_name(required);
    return {};
}

void draw_summary(const Editor &ui)
{
    const OperationPreset &p = ui.draft;
    ImGui::SeparatorText(local("目前設定", "Current selection"));
    const std::string planet = preview(ui.catalog.planet, p.planet, t(T_LAND_PICK_PLANET));
    ImGui::TextWrapped("%s  ·  %s", planet.c_str(), terrain_name(ui).c_str());
    ImGui::TextWrapped("%s  ·  %s  ·  %s", faction_name(p.faction), t(T_LBL_TEMPLATE),
                       preview(ui.catalog.task, p.template_index, t(T_TEMPLATE_GAME)).c_str());
    if (p.missions[0] || p.missions[1] || p.missions[2]) {
        std::string list;
        for (uint32_t i = 0; i < HOST_OPERATIONS_MISSION_SLOTS; ++i) {
            const MissionRow *row = p.missions[i] ? find_mission(ui.catalog, p.missions[i]) : nullptr;
            const char *official = row ? game(::mission_text[row->entry->row]) : nullptr;
            list += (i ? "  /  " : "") + (p.missions[i] ? official ? std::string(official) :
                                          row && *row->entry->name ? std::string(row->entry->name) :
                                          std::to_string(p.missions[i]) : std::string("—"));
        }
        ImGui::TextWrapped("%s%s", local("自選任務：", "Chosen missions: "), list.c_str());
    }
    ImGui::Text(t(T_EFFECT_COUNT), static_cast<unsigned>(p.effect_count), static_cast<unsigned>(HOST_OPERATIONS_MAX_EFFECTS));
    if (p.effect_count) {
        std::string ids;
        for (uint32_t i = 0; i < std::min(4u, p.effect_count); ++i)
            ids += (i ? ", #" : "#") + std::to_string(p.effects[i]);
        if (p.effect_count > 4) ids += "  (+" + std::to_string(p.effect_count - 4) + ")";
        ImGui::SameLine();
        ImGui::TextWrapped("%s", ids.c_str());
    }
}

struct Status {
    wchar_t text[512]{};
    bool editable = false, pending = false;
    void read() { host_ui_status(text, sizeof text / sizeof *text, &editable, &pending); }
};

constexpr ImU32 ink = IM_COL32(31, 28, 21, 255);

struct CommandStyle {
    ImU32 text, fill, hover, active, border;
};

constexpr CommandStyle primary = {ink, amber, IM_COL32(250,203,116,255), IM_COL32(210,153,62,255), amber};
constexpr CommandStyle secondary = {IM_COL32(244,224,185,255), IM_COL32(66,63,53,255), IM_COL32(88,80,60,255),
                                   IM_COL32(102,86,56,255), IM_COL32(144,125,85,255)};
constexpr CommandStyle withdraw = {IM_COL32(226,176,165,255), IM_COL32(42,39,39,255), IM_COL32(76,54,51,255),
                                  IM_COL32(94,60,55,255), IM_COL32(104,79,74,255)};

void action(Editor &ui, Status &status, Text label, MenuCommand command, const CommandStyle &style)
{
    const float em = ImGui::GetFontSize();
    const float width = std::max(5.6f * em, ImGui::CalcTextSize(t(label)).x + 1.5f * em);
    if (command != MENU_OPEN_TEMPORARILY) {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        const float gap = right - ImGui::GetItemRectMax().x - width;
        if (gap >= ImGui::GetStyle().ItemSpacing.x)
            ImGui::SameLine(0.0f, command == MENU_CLOSE_PLANET ? gap : ImGui::GetStyle().ItemSpacing.x);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, style.text);
    ImGui::PushStyleColor(ImGuiCol_Button, style.fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, style.active);
    ImGui::PushStyleColor(ImGuiCol_Border, style.border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    const bool clicked = ImGui::Button(t(label), ImVec2(width, 1.9f * em));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
    if (!clicked) return;
    const bool accepted = host_ui_submit(command, &ui.draft);
    status.read();
    ui.feedback = T_COUNT;
    if (!accepted) ui.report(status.pending ? T_SUBMIT_PENDING : T_SUBMIT_UNAVAILABLE);
    else if (!*status.text) ui.report(T_SUBMIT_SENT);
}

// Sync code: the host copies the whole selection; a friend with this mod pastes it and this game applies the same
// changes (also as a joiner, before or after joining).
void draw_sync_code(Editor &ui, Status &status, bool valid_selection)
{
    ImGui::SeparatorText(local("同步碼（給也裝了本模組的朋友）", "Sync code (for friends with this mod)"));
    ImGui::BeginDisabled(!valid_selection);
    if (ImGui::Button(local("複製同步碼", "Copy sync code"))) {
        char code[256];
        if (menu_preset_code(&ui.draft, code, sizeof code)) {
            ImGui::SetClipboardText(code);
            ui.code_message = std::string(local("已複製：", "Copied: ")) + code;
        } else {
            ui.code_message = local("目前的設定無法轉成同步碼。", "The current selection can't be turned into a sync code.");
        }
        ui.code_message_until = ImGui::GetTime() + 8.0;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!status.editable || status.pending);
    if (ImGui::Button(local("貼上同步碼並套用", "Paste sync code and apply"))) {
        const char *text = ImGui::GetClipboardText();
        OperationPreset pasted{};
        if (text && menu_parse_code(text, &pasted)) {
            ui.draft = pasted;
            ui.planets.needs_choice = ui.terrains.needs_choice = ui.tasks.needs_choice = false;
            const bool accepted = host_ui_submit(MENU_OPEN_TEMPORARILY, &ui.draft);
            status.read();
            ui.code_message = accepted ? local("已套用同步碼的全部設定。", "All settings of the sync code are applied.") :
                                         local("已載入同步碼，但目前無法套用，請稍後再按一次。",
                                               "The sync code is loaded but can't be applied yet; press again later.");
        } else {
            ui.code_message = local("同步碼無效：請確認整段（含 HF1-）都有複製。", "Invalid sync code: copy all of it, including HF1-.");
        }
        ui.code_message_until = ImGui::GetTime() + 8.0;
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", local("朋友貼上後，他的遊戲會套用同樣的星球、地形、任務組、任務 1–3 和詞條；加入房間前後都可以。撤回請按「撤回開放」。",
                                    "After pasting, a friend's game applies the same planet, terrain, template, missions 1-3 and "
                                    "effects, before or after joining. Use Withdraw to stop."));
    if (!ui.code_message.empty() && ImGui::GetTime() < ui.code_message_until)
        ImGui::TextWrapped("%s", ui.code_message.c_str());
}

void draw_contents(Editor &ui)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", t(T_SHORTCUT));
    const float lang_width = ImGui::CalcTextSize("繁體中文").x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(0, std::max(ImGui::GetStyle().ItemSpacing.x,
                                ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x -
                                ImGui::GetItemRectMax().x - lang_width));
    static const char *const languages[LANG_COUNT] = {"繁體中文", "English", "简体中文"};
    ImGui::SetNextItemWidth(lang_width);
    if (ImGui::BeginCombo("##language", languages[ui_language()], ImGuiComboFlags_NoArrowButton)) {
        for (Language option : {LANG_ZH, LANG_ZH_HANS, LANG_EN})
            if (ImGui::Selectable(languages[option], ui_language() == option)) {
                set_ui_language(option);
                menu_save_language();
            }
        ImGui::EndCombo();
    }
    ui.catalog.refresh();
    Status status;
    status.read();
    ImGui::BeginDisabled(!status.editable || status.pending);
    if (ImGui::BeginTabBar("settings", ImGuiTabBarFlags_DrawSelectedOverline)) {
        const std::string planet = std::string(t(T_LBL_PLANET)) + "###planet";
        const std::string terrain = std::string(t(T_LBL_BIOME)) + "###terrain";
        const std::string operation = std::string(t(T_ACTION_TITLE)) + "###operation";
        const std::string effects = std::string(t(T_EFFECT_TITLE)) + "###effects";
        if (ImGui::BeginTabItem(planet.c_str())) {
            choice_list(ListKind::Planet, ui.planets, ui.catalog.planet, ui.draft.planet);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(terrain.c_str())) {
            choice_list(ListKind::Terrain, ui.terrains, ui.catalog.terrain, ui.draft.biome);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(operation.c_str())) { draw_operation(ui); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(effects.c_str())) { draw_effects(ui); ImGui::EndTabItem(); }
        const std::string missions = std::string(local("自定義任務", "Custom missions")) + "###missions";
        if (ImGui::BeginTabItem(missions.c_str())) { draw_missions(ui); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    draw_summary(ui);

    const auto error = problem(ui);
    ImGui::PushStyleColor(ImGuiCol_Text, amber);
    ImGui::SeparatorText(local("戰術桌狀態", "War table status"));
    if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    ImGui::PopStyleColor();
    const std::string report = utf8(status.text);
    if (!report.empty()) ImGui::TextWrapped("%s", report.c_str());
    const bool feedback = ui.feedback != T_COUNT && ImGui::GetTime() < ui.feedback_until;
    const Text state = !status.editable ? T_STATUS_WAIT : status.pending ? T_SUBMIT_PENDING : T_STATUS_READY;
    if (((report.empty() && error.empty() && !feedback) || !status.editable || status.pending) && report != t(state))
        ImGui::TextWrapped("%s", t(state));
    if (feedback && report != t(ui.feedback) && state != ui.feedback) {
        const char *message = ui.feedback == T_EFFECT_TOO_MANY ?
            local("加入後會超過 32 個 ID，已保留原選擇。", "Adding this selection would exceed 32 IDs.") : t(ui.feedback);
        ImGui::TextWrapped("%s", message);
    }

    ImGui::BeginDisabled(!status.editable || status.pending);
    ImGui::BeginDisabled(!error.empty());
    action(ui, status, T_BTN_TEMPORARY, MENU_OPEN_TEMPORARILY, primary);
    action(ui, status, T_BTN_SAVE, MENU_SAVE_AND_OPEN, secondary);
    ImGui::EndDisabled();
    action(ui, status, T_BTN_CLOSE, MENU_CLOSE_PLANET, withdraw);
    ImGui::EndDisabled();
    draw_sync_code(ui, status, error.empty());
}

// Scoped to this menu, including its children and combo popups.
struct MenuTheme {
    struct Color { ImGuiCol slot; ImU32 value; };
    static constexpr Color colors[] = {
        {ImGuiCol_Text,              IM_COL32(237,236,229,255)}, {ImGuiCol_TextDisabled,      IM_COL32(164,165,156,255)},
        {ImGuiCol_WindowBg,          IM_COL32(29,31,32,253)},    {ImGuiCol_ChildBg,           IM_COL32(22,24,25,255)},
        {ImGuiCol_PopupBg,           IM_COL32(40,42,42,255)},    {ImGuiCol_Border,            IM_COL32(77,80,78,255)},
        {ImGuiCol_TitleBg,           IM_COL32(44,46,45,255)},    {ImGuiCol_TitleBgActive,     IM_COL32(49,51,49,255)},
        {ImGuiCol_FrameBg,           IM_COL32(51,54,54,255)},    {ImGuiCol_FrameBgHovered,    IM_COL32(65,68,65,255)},
        {ImGuiCol_FrameBgActive,     IM_COL32(75,73,64,255)},    {ImGuiCol_TextSelectedBg,    IM_COL32(160,127,69,160)},
        {ImGuiCol_Tab,               IM_COL32(42,45,45,255)},    {ImGuiCol_TabHovered,        IM_COL32(79,75,61,255)},
        {ImGuiCol_TabSelected,       IM_COL32(77,70,53,255)},    {ImGuiCol_TabSelectedOverline,amber},
        {ImGuiCol_Button,            IM_COL32(57,60,58,255)},    {ImGuiCol_ButtonHovered,     IM_COL32(74,76,70,255)},
        {ImGuiCol_ButtonActive,      IM_COL32(89,85,71,255)},    {ImGuiCol_Header,            IM_COL32(84,72,48,255)},
        {ImGuiCol_HeaderHovered,     IM_COL32(74,73,63,255)},    {ImGuiCol_HeaderActive,      IM_COL32(105,87,53,255)},
        {ImGuiCol_CheckMark, amber}, {ImGuiCol_NavCursor, amber}, {ImGuiCol_Separator,        IM_COL32(78,80,74,255)},
        {ImGuiCol_ResizeGrip,        IM_COL32(120,115,95,140)},  {ImGuiCol_ResizeGripHovered, amber},
        {ImGuiCol_ResizeGripActive,  amber}
    };
    explicit MenuTheme(float em) {
        for (const Color &c : colors) ImGui::PushStyleColor(c.slot, c.value);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(.88f * em, .6f * em));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(.58f * em, .25f * em));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(.6f * em, .34f * em));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, .13f * em);
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, .13f * em);
        ImGui::PushStyleVar(ImGuiStyleVar_SeparatorTextPadding, ImVec2(0, .18f * em));
    }
    ~MenuTheme() {
        ImGui::PopStyleVar(6);
        ImGui::PopStyleColor(static_cast<int>(sizeof colors / sizeof *colors));
    }
};

} // namespace

extern "C" void host_ui_draw(bool *open)
{
    if (!open || !*open) return;
    // F11 already toggles visibility on the game thread. Handling its opening key here would close the menu twice.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { *open = false; return; }
    static Editor ui;
    const ImGuiViewport *view = ImGui::GetMainViewport();
    const float em = ImGui::GetFontSize();
    MenuTheme theme(em);
    ImGui::SetNextWindowPos(view->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(43 * em, view->WorkSize.x), std::min(34 * em, view->WorkSize.y)),
                             ImGuiCond_FirstUseEver);
    const std::string title = std::string(t(T_WINDOW)) + "###hostOperations";
    if (ImGui::Begin(title.c_str(), open, ImGuiWindowFlags_NoCollapse)) draw_contents(ui);
    ImGui::End();
}
