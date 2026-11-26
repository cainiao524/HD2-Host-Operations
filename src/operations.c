#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "hellforge_mod.h"
#include "effects.h"
#include "i18n.h"
#include "overlay.h"

/* Game build 25480438. The refresh routine copies the live war at the
 * beginning of the board into its campaign snapshot, then builds operations. */
enum {
    BuildOperationRow = 0x11e3250,
    RefreshBoard = 0x12d58e0,
    BoardPointer = 0x347cee8,
    TemplateTable = 0x32fef10,
    TemplateStride = 0x490,
    TemplateCount = 25,
    /* Picks the candidates for an operation's next mission: (container, board, operation row, template, planet, ?, ?,
     * difficulty, &seed, out mission-table indices) -> count. The operation row's +0x58 is the number of missions it
     * already has, so it is the slot being filled. Called by the mission generator 0x11e5670 and two others. */
    SelectMissions = 0x11e5100,
    OperationMissionCount = 0x58,
    MissionTable = 0x3773420,
    MissionStride = 0x380,
    MissionCount = 162,
    CampaignSnapshot = 0x101438,
    DefinitionStride = 0x118,
    DefinitionSettingsHash = 0x18,
    DefinitionCount = 0x23014,
    WorldFaction = 0x46044,
    WorldEnabled = 0x46050,
    WorldEffects = 0x460e8,
    WorldEffectCount = 0x46168,
    WorldStride = 0x130,
    CampaignRows = 0x7004c,
    CampaignCount = 0x7284c,
    CampaignLimit = 512,
    NativeOperations = 0xf7280,
    OperationStride = 92,
    OperationLimit = 110,
    NativeActive = 0x78e88
};

typedef struct CampaignRow {
    uint32_t id, count, planet, kind, race;
} CampaignRow;

typedef struct World {
    uint32_t faction, effect_count;
    uint8_t enabled;
    uint32_t effects[HOST_OPERATIONS_MAX_EFFECTS];
} World;

/* Reversible fields for a board refresh or the lifetime of the selected operation. */
typedef struct Change {
    uintptr_t address;
    size_t size;
    unsigned char before[sizeof(((World *)0)->effects)];
    unsigned char after[sizeof(((World *)0)->effects)];
} Change;

typedef struct Overlay {
    Change changes[7];
    size_t count;
} Overlay;

typedef struct ArmedOperation {
    OperationPreset preset;
    uint32_t settings_hash;
    uint32_t template_id;
    /* Missions 1-3 by mission-table index (UINT32_MAX: the game's own pick) and their IDs, for the log. */
    uint32_t mission_index[HOST_OPERATIONS_MISSION_SLOTS];
    uint32_t mission_id[HOST_OPERATIONS_MISSION_SLOTS];
    uint32_t mission_count;
    uint8_t mission_logged;
    bool row_verified, row_failed;
    uint32_t serial;
    bool enabled;
    bool saved;
    bool saw_action;
} ArmedOperation;

_Static_assert(sizeof(CampaignRow) == 20, "campaign row layout");
_Static_assert(CampaignRows + CampaignLimit * sizeof(CampaignRow) == CampaignCount, "campaign capacity");
_Static_assert(WorldEffects + sizeof(((World *)0)->effects) == WorldEffectCount, "world effect capacity");

static const HfApi *api;
static HfMod *self;
static uintptr_t game;
static void *original_refresh, *original_build, *original_select;
static bool language_loaded;
static DWORD game_thread;
static bool hook_ready, hook_failed, invoked_refresh, snapshot_applied;
static bool restore_failed, key_was_down, could_edit, was_on_ship, saved_checked;
static uint32_t last_role = UINT32_MAX;
static uint32_t next_serial;
static ArmedOperation armed;
static bool request_queued;
static OperationPreset queued_preset;
static MenuCommand queued_command;

static bool read_bytes(uintptr_t address, void *out, size_t size)
{
    return address && api->read(address, out, size) == 0;
}

static uintptr_t game_pointer(unsigned offset)
{
    uintptr_t pointer = 0;
    read_bytes(game + offset, &pointer, sizeof pointer);
    return pointer;
}

static bool is_host(void)
{
    HfSession session = { .role = HF_SESSION_JOINER };
    api->session(self, &session);
    return session.role == HF_SESSION_SOLO || session.role == HF_SESSION_HOST;
}

static const PlanetEntry *find_planet(uint32_t index)
{
    for (size_t i = 0; i < planet_count; ++i)
        if (planets[i].index == index) return &planets[i];
    return NULL;
}

static Overlay active_changes;
static uintptr_t active_board;
static uint32_t active_revision;
static bool active_write_reported, recheck_active_world;

static uintptr_t definition_hash_address(uintptr_t base, uint32_t planet)
{
    return base + DefinitionStride * planet + DefinitionSettingsHash;
}

static bool world_identity(uintptr_t base, uint32_t planet, uint32_t expected_hash)
{
    uint32_t count, hash;
    if (!read_bytes(base + DefinitionCount, &count, sizeof count) || count > CampaignLimit || planet >= count ||
        !read_bytes(definition_hash_address(base, planet), &hash, sizeof hash))
        return false;
    /* While this mod keeps another settingsHash on the planet, that one is the planet's identity on the live board. */
    return hash == expected_hash || (active_board == base && armed.enabled && armed.serial == active_revision && planet == armed.preset.planet && hash == armed.preset.biome);
}

static bool read_world(uintptr_t base, uint32_t planet, World *world)
{
    uintptr_t row = base + WorldStride * planet;
    return read_bytes(row + WorldFaction, &world->faction, sizeof world->faction) &&
           world->faction <= 4 &&
           read_bytes(row + WorldEnabled, &world->enabled, sizeof world->enabled) &&
           world->enabled <= 1 &&
           read_bytes(row + WorldEffectCount, &world->effect_count, sizeof world->effect_count) &&
           world->effect_count <= HOST_OPERATIONS_MAX_EFFECTS &&
           read_bytes(row + WorldEffects, world->effects, sizeof world->effects);
}

static bool snapshot_has_effects(const OperationPreset *preset, const World *world)
{
    for (uint32_t i = 0; i < preset->effect_count; ++i)
        if (effect_position(world->effects, world->effect_count, preset->effects[i]) == world->effect_count)
            return false;
    return true;
}

static const wchar_t *merge_effects(World *world, const OperationPreset *preset)
{
    if (preset->effect_count > HOST_OPERATIONS_MAX_EFFECTS)
        return tr(T_OP_MAX_EFFECTS);
    if (!preset->effect_count)
        return NULL;

    /* Keep the world's original order and any IDs outside the menu catalog. */
    uint32_t count = 0;
    for (uint32_t i = 0; i < world->effect_count; ++i)
        append_effect_id(world->effects, &count, world->effects[i]);
    for (uint32_t i = 0; i < preset->effect_count; ++i) {
        const uint32_t id = preset->effects[i]; /* any ID: the catalogue only names some of them */
        if (!append_effect_id(world->effects, &count, id))
            return tr(T_OP_TOO_MANY);
    }
    world->effect_count = count;
    return NULL;
}

static bool read_campaigns(uintptr_t base, CampaignRow rows[CampaignLimit], uint32_t *count)
{
    return read_bytes(base + CampaignCount, count, sizeof *count) && *count <= CampaignLimit &&
           (!*count || read_bytes(base + CampaignRows, rows, *count * sizeof rows[0]));
}

static bool campaign_present(uintptr_t base, uint32_t planet)
{
    CampaignRow rows[CampaignLimit];
    uint32_t count;
    if (!read_campaigns(base, rows, &count)) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (rows[i].planet == planet) return true;
    return false;
}

static bool stage_change(Overlay *overlay, uintptr_t address, const void *new_data, size_t size)
{
    if (overlay->count >= sizeof overlay->changes / sizeof overlay->changes[0] ||
        size > sizeof overlay->changes[0].before) return false;
    Change *change = &overlay->changes[overlay->count];
    if (!read_bytes(address, change->before, size)) return false;
    if (!memcmp(change->before, new_data, size)) return true;
    change->address = address;
    change->size = size;
    memcpy(change->after, new_data, size);
    ++overlay->count;
    return true;
}

static void restore_changes(const Overlay *overlay, size_t applied)
{
    while (applied) {
        const Change *change = &overlay->changes[--applied];
        unsigned char current[sizeof change->after];
        if (!read_bytes(change->address, current, change->size)) {
            restore_failed = true;
        } else if (!memcmp(current, change->after, change->size) &&
                   api->write(change->address, change->before, change->size)) {
            restore_failed = true;
        }
    }
}

static bool write_changes(const Overlay *overlay)
{
    for (size_t i = 0; i < overlay->count; ++i) {
        const Change *change = &overlay->changes[i];
        if (api->write(change->address, change->after, change->size)) {
            restore_changes(overlay, i);
            return false;
        }
    }
    return true;
}

static void restore_active_world(uintptr_t board)
{
    if (board && active_board == board) restore_changes(&active_changes, active_changes.count);
    active_board = 0;
    active_changes.count = 0;
    active_revision = 0;
}

static const wchar_t *stage_world(uintptr_t board, const OperationPreset *p, World *world, Overlay *overlay)
{
    const uintptr_t world_row = board + WorldStride * p->planet;
    const wchar_t *error = merge_effects(world, p);
    if (error) return error;
    const uint8_t enabled = 1;
    if ((p->biome && !stage_change(overlay, definition_hash_address(board, p->planet), &p->biome, sizeof p->biome)) ||
        !stage_change(overlay, world_row + WorldEnabled, &enabled, sizeof enabled) ||
        !stage_change(overlay, world_row + WorldFaction, &p->faction, sizeof p->faction))
        return tr(T_OP_INVALID);
    if (p->effect_count &&
        (!stage_change(overlay, world_row + WorldEffects, world->effects, world->effect_count * sizeof world->effects[0]) ||
         !stage_change(overlay, world_row + WorldEffectCount, &world->effect_count, sizeof world->effect_count)))
        return tr(T_OP_INVALID);
    return NULL;
}

static const wchar_t *stage_overlay(uintptr_t board, const ArmedOperation *action, Overlay *overlay)
{
    const OperationPreset *p = &action->preset;
    World world;
    uint32_t count;
    CampaignRow list[CampaignLimit];
    const wchar_t *invalid = tr(T_OP_INVALID);
    if (!world_identity(board, p->planet, action->settings_hash) ||
        !read_world(board, p->planet, &world) || !read_campaigns(board, list, &count)) return invalid;
    bool present = false, id_used = false;
    const uint32_t new_id = 900000u + p->planet;
    for (uint32_t i = 0; i < count; ++i) {
        if (list[i].planet == p->planet) present = true;
        if (list[i].id == new_id) id_used = true;
    }
    if (!present) {
        if (id_used || count == CampaignLimit) return invalid;
        const CampaignRow row = {new_id, 1, p->planet, 0, 1};
        if (!stage_change(overlay, board + CampaignRows + count * sizeof row, &row, sizeof row)) return invalid;
        ++count;
        if (!stage_change(overlay, board + CampaignCount, &count, sizeof count)) return invalid;
    }

    return stage_world(board, p, &world, overlay);
}

typedef uintptr_t (*RefreshRoutine)(uintptr_t board);

static uintptr_t refresh_detour(uintptr_t board)
{
    Overlay overlay = {0};
    size_t applied = 0;
    const bool apply = GetCurrentThreadId() == game_thread && armed.enabled && !restore_failed &&
                       is_host() && api->game_state(self) == HF_STATE_SHIP && board == game_pointer(BoardPointer);
    bool good = apply && stage_overlay(board, &armed, &overlay) == NULL && write_changes(&overlay);
    if (good) applied = overlay.count;
    const uintptr_t result = ((RefreshRoutine)original_refresh)(board);
    restore_changes(&overlay, applied);
    if (GetCurrentThreadId() == game_thread && active_board == board) recheck_active_world = true;
    if (invoked_refresh && apply) {
        World snapshot;
        uintptr_t copy = board + CampaignSnapshot;
        uint32_t snapshot_hash = 0;
        snapshot_applied = good && !restore_failed &&
                           read_bytes(definition_hash_address(copy, armed.preset.planet), &snapshot_hash, sizeof snapshot_hash) &&
                           snapshot_hash == (armed.preset.biome ? armed.preset.biome : armed.settings_hash) &&
                           campaign_present(copy, armed.preset.planet) &&
                           read_world(copy, armed.preset.planet, &snapshot) && snapshot.enabled == 1 &&
                           snapshot_has_effects(&armed.preset, &snapshot) &&
                           snapshot.faction == armed.preset.faction;
    }
    return result;
}

static uint32_t mission_index_of(uint32_t id)
{
    for (uint32_t i = 0; i < MissionCount; ++i) {
        uint32_t row_id = 0;
        if (read_bytes(game + MissionTable + (uintptr_t)i * MissionStride, &row_id, sizeof row_id) && row_id == id) return i;
    }
    return UINT32_MAX;
}

typedef uint32_t (*SelectRoutine)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                  uintptr_t, uintptr_t);

/* The game's candidates for the entry planet's mission n become the host's mission n alone. */
static uint32_t select_detour(uintptr_t container, uintptr_t board, uintptr_t operation, uintptr_t template_row,
                              uintptr_t planet, uintptr_t a6, uintptr_t a7, uintptr_t difficulty, uintptr_t seed, uintptr_t out)
{
    const uint32_t count = ((SelectRoutine)original_select)(container, board, operation, template_row, planet, a6, a7,
                                                            difficulty, seed, out);
    if (!armed.enabled || !armed.mission_count || restore_failed || !out || !operation) return count;
    uint16_t row_planet;
    uint8_t slot;
    if (!read_bytes(operation + 16, &row_planet, sizeof row_planet) || row_planet != armed.preset.planet ||
        !read_bytes(operation + OperationMissionCount, &slot, sizeof slot) || slot >= HOST_OPERATIONS_MISSION_SLOTS ||
        armed.mission_index[slot] == UINT32_MAX)
        return count; /* host or joiner: a friend with the same sync code replaces the same missions */
    const uint32_t index = armed.mission_index[slot];
    uint32_t first = UINT32_MAX;
    read_bytes(out, &first, sizeof first);
    if (api->write(out, &index, sizeof index)) return count;
    if (!(armed.mission_logged & (1u << slot))) {
        armed.mission_logged |= (uint8_t)(1u << slot);
        api->log(self, "Mission %u of planet %u: mission %u (table row %u) replaces the game's %u candidate(s) (first row %d), "
                 "difficulty %u, thread %s", slot + 1, row_planet, armed.mission_id[slot], index, count, (int)first,
                 (unsigned)difficulty, GetCurrentThreadId() == game_thread ? "game" : "other");
    }
    return 1;
}

/* The game resolves an explicit settings ID before generating the missions from the seed.
 * Only its candidate array belongs to the editable board. Reconstructed active operations
 * and unrelated callers of the same routine must use their original input. */
typedef bool (*BuildRowRoutine)(uintptr_t row);

static bool build_row_detour(uintptr_t row)
{
    BuildRowRoutine build = (BuildRowRoutine)original_build;
    if (GetCurrentThreadId() != game_thread || !armed.enabled || !armed.template_id || restore_failed ||
        api->game_state(self) != HF_STATE_SHIP || !is_host()) return build(row);
    uintptr_t board = game_pointer(BoardPointer);
    uintptr_t first = board + NativeOperations;
    if (!board || row < first || row - first >= OperationLimit * OperationStride ||
        (row - first) % OperationStride != 0) return build(row);

    unsigned char before[OperationStride];
    uint16_t planet;
    if (!read_bytes(row, before, sizeof before)) return build(row);
    memcpy(&planet, before + 16, sizeof planet);
    /* Every difficulty of the entry planet takes the template; the war table and the game decide the difficulty.
     * Where the game cannot build the template, the row is put back and built as it was (below). */
    if (planet != armed.preset.planet) return build(row);

    Overlay input = {0};
    if (stage_change(&input, row + 8, &armed.template_id, sizeof armed.template_id) &&
        stage_change(&input, row + 36, &armed.preset.faction, sizeof armed.preset.faction) && write_changes(&input)) {
        bool valid = build(row);
        uint32_t index, race;
        if (valid && read_bytes(row + 56, &index, sizeof index) && index == armed.preset.template_index &&
            read_bytes(row + 36, &race, sizeof race) && race == armed.preset.faction) {
            if (!armed.row_verified) {
                armed.row_verified = true;
                if (!invoked_refresh) {
                    wchar_t confirmed[128];
                    swprintf(confirmed, ARRAYSIZE(confirmed),
                             loc(L"星球 #%u 已產生模板 %u 的任務卡（D%u）。", L"Planet #%u generated a card with template %u (D%u)."),
                             planet, index, before[32]);
                    menu_report(confirmed);
                }
                api->log(self, "Generated native candidate: planet=%u template=%u settingsId=%u difficulty=%u",
                         planet, index, armed.template_id, before[32]);
            }
            return valid;
        }
        if (api->write(row, before, sizeof before)) {
            restore_failed = true;
            return valid;
        }
    }
    if (!armed.row_failed) {
        armed.row_failed = true;
        api->log(self, "Candidate generation failed for planet=%u template=%u difficulty=%u", planet, armed.preset.template_index, before[32]);
        menu_report(loc(L"遊戲無法用選定的任務組產生這張任務卡，該卡維持原版。", L"The game could not generate this template for that card; the card keeps the game's own."));
    }
    return build(row);
}

static bool setup_native(void)
{
    size_t size = 0;
    game = api->module_base("game.dll", &size);
    if (!game) return false;
    if (size <= BoardPointer + sizeof(uintptr_t) ||
        !api->code_matches(game + RefreshBoard, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57") ||
        !api->code_matches(game + 0x12d5910, "41 B8 E8 8E 07 00 48 81 C1 38 14 10 00") ||
        !api->code_matches(game + 0x11e32ed, "48 8B 0D F4 9B 29 02") ||
        !api->code_matches(game + 0x133ce24, "49 8D 81 84 14 17 00") ||
        !api->code_matches(game + 0x1267ac2, "44 8B 87 84 3C 17 00")) {
        hook_failed = true;
        api->log(self, "Unsupported campaign reader or refresh function; installation skipped");
        menu_report(tr(T_OP_BUILD_MISMATCH));
        return false;
    }
    if (api->hook(self, game + RefreshBoard, (void *)refresh_detour, &original_refresh)) {
        hook_failed = true;
        api->log(self, "Campaign refresh hook failed");
        menu_report(tr(T_OP_HOOK_FAILED));
        return false;
    }
    if (api->code_matches(game + BuildOperationRow, "48 8B C4 48 89 58 18 48 89 70 20 55 57 41 55") &&
        api->code_matches(game + 0x11e328b, "8B 41 08 48 8D 35 7B BC 11 02 8B 59 0C") &&
        api->code_matches(game + 0x11e3343, "41 89 4D 38 E9 7A 01 00 00") &&
        api->code_matches(game + 0x11e3b5d, "41 83 7D 38 FF 0F 95 C0 41 88 45 34")) {
        if (api->hook(self, game + BuildOperationRow, (void *)build_row_detour, &original_build)) original_build = NULL;
    }
    api->log(self, "Per-planet template generation: %s", original_build ? "ready" : "unavailable");
    if (api->code_matches(game + SelectMissions, "40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC C8 00 00 00") &&
        api->code_matches(game + 0x11e5219, "0F B6 43 58") &&
        api->code_matches(game + 0x11e5b82, "41 C6 46 58 00") &&
        api->code_matches(game + 0x11e5bf4, "E8 07 F5 FF FF")) {
        if (api->hook(self, game + SelectMissions, (void *)select_detour, &original_select)) original_select = NULL;
    }
    api->log(self, "Chosen missions 1-3: %s", original_select ? "ready" : "unavailable");
    api->log(self, "Native campaign refresh ready");
    return true;
}

static bool refresh(uintptr_t board)
{
    if (!hook_ready || !board || restore_failed) return false;
    uint32_t count;
    if (!read_bytes(board + DefinitionCount, &count, sizeof count) || !count || count > CampaignLimit)
        return false;
    invoked_refresh = true;
    snapshot_applied = false;
    ((RefreshRoutine)(game + RefreshBoard))(board);
    invoked_refresh = false;
    return snapshot_applied;
}

static bool action_active(uintptr_t board, uint32_t *planet)
{
    unsigned char row[OperationStride];
    if (!read_bytes(board ? board + NativeActive : 0, row, sizeof row) || !row[52]) return false;
    uint16_t index;
    memcpy(&index, row + 16, sizeof index);
    *planet = index;
    return true;
}

static const wchar_t *arm(uintptr_t board, const OperationPreset *preset, bool saved)
{
    if (!hook_ready || !board || restore_failed) return tr(T_OP_NOT_READY);
    if (preset->effect_count > HOST_OPERATIONS_MAX_EFFECTS) return tr(T_OP_CHECK);
    /* A joiner (a friend who pasted the host's sync code) applies the same changes to its own game; the host-only
     * steps (campaign row, war table rebuild) are skipped. */
    const bool joiner = !is_host();
    const PlanetEntry *target = find_planet(preset->planet);
    if (!target) return tr(T_OP_PLANET);
    ArmedOperation next = {0};
    World world;
    if (!world_identity(board, target->index, target->settings_hash) || !read_world(board, target->index, &world)) {
        static wchar_t missing[200];
        swprintf(missing, ARRAYSIZE(missing),
                 loc(L"目前戰況缺少與 #%u 相符的入口。請改選入口星球，仍可保留所選地形。", L"Planet #%u has no matching entry on this war board. Pick another entry planet. You can keep the terrain."),
                 target->index);
        api->log(self, "Unavailable entry planet=%u expectedHash=%u", target->index, target->settings_hash);
        return missing;
    }
    uint32_t faction = preset->faction;
    if (preset->template_index != HOST_OPERATIONS_NO_TEMPLATE) {
        uint32_t header[5];
        if (!original_build || preset->template_index >= TemplateCount ||
            !read_bytes(game + TemplateTable + preset->template_index * TemplateStride, header, sizeof header) ||
            !header[0] || header[2] < 2 || header[2] > 4)
            return loc(L"此構建無法讀取所選任務組。", L"The game template is unavailable on this build.");
        if (faction && faction != header[2])
            return loc(L"請將陣營設為自動，或選擇任務組所屬陣營。", L"Choose Auto faction or the faction of the selected template.");
        faction = header[2];
        next.template_id = header[0];
    }
    for (uint32_t i = 0; i < HOST_OPERATIONS_MISSION_SLOTS; ++i) {
        next.mission_index[i] = UINT32_MAX;
        if (!preset->missions[i]) continue;
        const uint32_t index = mission_index_of(preset->missions[i]);
        if (!original_select || index == UINT32_MAX)
            return loc(L"此版本無法使用自選任務。", L"Chosen missions are unavailable on this build.");
        next.mission_index[i] = index;
        next.mission_id[i] = preset->missions[i];
        ++next.mission_count;
    }
    if (!faction) faction = world.faction >= 2 ? world.faction : planet_default_faction(target);
    if (!faction) faction = 2; /* A neutral entry needs an enemy to generate operation cards. */
    if (faction < 2 || faction > 4) return tr(T_OP_FACTION);
    next.preset = *preset;
    next.preset.planet = target->index;
    next.preset.faction = faction;
    next.settings_hash = target->settings_hash;
    next.serial = ++next_serial;
    next.enabled = true;
    next.saved = saved;

    Overlay check = {0};
    const wchar_t *error = joiner ? NULL : stage_overlay(board, &next, &check);
    if (error) return error;
    restore_active_world(board);
    if (restore_failed) return tr(T_OP_NOT_READY);
    ArmedOperation previous = armed;
    armed = next;
    if (!joiner && !refresh(board)) {
        restore_active_world(board);
        armed = previous;
        refresh(board);
        return tr(T_OP_SNAPSHOT);
    }
    if (saved && !menu_save_preset(preset)) {
        restore_active_world(board);
        armed = previous;
        if (!joiner) refresh(board);
        return tr(T_OP_SAVE);
    }
    recheck_active_world = true;
    char ids[HOST_OPERATIONS_MAX_EFFECTS * 11 + 1] = "";
    size_t used = 0;
    for (uint32_t i = 0; i < preset->effect_count; ++i)
        used += (size_t)snprintf(ids + used, sizeof ids - used, "%s%u", i ? "," : "", preset->effects[i]);
    api->log(self, "%s: planet=%u hash=%u faction=%u template=%u missions=%u,%u,%u selected_effect_ids=[%s] revision=%u",
             joiner ? "Joiner follows the sync code" : "Host snapshot applied", target->index,
             preset->biome ? preset->biome : target->settings_hash, faction, preset->template_index, preset->missions[0],
             preset->missions[1], preset->missions[2], ids, next.serial);
    static wchar_t message[256];
    wchar_t name[96];
    if (!MultiByteToWideChar(CP_UTF8, 0, target->name, -1, name, sizeof name / sizeof name[0]))
        swprintf(name, sizeof name / sizeof name[0], tr(T_OP_PLANET_N), target->index);
    wchar_t extras[96] = L"";
    if (preset->biome)
        swprintf(extras + wcslen(extras), ARRAYSIZE(extras) - wcslen(extras), tr(T_OP_EXTRA_TERRAIN), preset->biome);
    if (next.template_id)
        swprintf(extras + wcslen(extras), ARRAYSIZE(extras) - wcslen(extras), tr(T_OP_EXTRA_TEMPLATE), preset->template_index);
    if (next.mission_count)
        swprintf(extras + wcslen(extras), ARRAYSIZE(extras) - wcslen(extras),
                 loc(L"，自選任務 %u 個", L", %u chosen mission(s)"), next.mission_count);
    swprintf(message, sizeof message / sizeof message[0],
             tr(T_OP_OPENED), name, target->index, extras, preset->effect_count);
    if (next.template_id) {
        const wchar_t *detail = armed.row_failed ?
            (loc(L" 部分任務卡產生失敗。", L" Some requested cards failed to generate.")) :
            armed.row_verified ?
            (loc(L" 已核對產生的任務卡模板。", L" Selected template verified on a generated card.")) :
            (loc(L" 等待戰術桌產生任務卡。", L" Waiting for the war table to generate a card."));
        wcsncat(message, detail, ARRAYSIZE(message) - wcslen(message) - 1);
    }
    return message;
}

static int template_of_id(uint32_t id)
{
    for (uint32_t i = 0; i < TemplateCount; ++i) {
        uint32_t row_id = 0;
        if (read_bytes(game + TemplateTable + i * TemplateStride, &row_id, sizeof row_id) && row_id == id) return (int)i;
    }
    return -1;
}

static bool json_number(const char *text, const char *key, long long *out)
{
    char pattern[64];
    snprintf(pattern, sizeof pattern, "\"%s\"", key);
    const char *at = strstr(text, pattern);
    if (!at || !(at = strchr(at + strlen(pattern), ':'))) return false;
    *out = _strtoi64(at + 1, NULL, 10);
    return true;
}

#define MAX_SEEN_BODIES 256
static char seen_bodies[MAX_SEEN_BODIES][64];
static int seen_body_count;
static float body_wait;

/* The operations this machine's game created, as HellForge's backend kept them (<session>\backend\mission-bodies). */
static void scan_operation_records(void)
{
    if (!game || api->size < offsetof(HfApi, logs_folder) + sizeof api->logs_folder) return;
    const char *logs = api->logs_folder(self);
    char pattern[1024];
    wchar_t wide[1024];
    snprintf(pattern, sizeof pattern, "%s\\backend\\mission-bodies\\*-Operation_Create.json", logs ? logs : "");
    if (!logs || !*logs || !MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wide, 1024)) return;
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(wide, &found);
    if (find == INVALID_HANDLE_VALUE) return;
    wchar_t *slash = wcsrchr(wide, L'\\');
    const size_t directory = slash ? (size_t)(slash - wide) + 1 : 0;
    do {
        char name[64];
        if (!WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        bool seen = false;
        for (int i = 0; i < seen_body_count && !seen; ++i) seen = strcmp(seen_bodies[i], name) == 0;
        if (seen || seen_body_count == MAX_SEEN_BODIES) continue;
        wchar_t file_path[1100];
        wcsncpy(file_path, wide, directory);
        file_path[directory] = 0;
        wcsncat(file_path, found.cFileName, ARRAYSIZE(file_path) - directory - 1);
        FILE *file = _wfopen(file_path, L"rb");
        if (!file) continue;
        char text[4096];
        const size_t n = fread(text, 1, sizeof text - 1, file);
        fclose(file);
        text[n] = 0;
        long long settings = 0, seed = 0, planet = -1, difficulty = 0, race = 0, region = -1;
        if (!json_number(text, "operationSettingsId", &settings)) continue;
        if (!strrchr(text, '}')) continue;
        memcpy(seen_bodies[seen_body_count++], name, sizeof name);
        json_number(text, "seed", &seed);
        json_number(text, "planetIndex", &planet);
        json_number(text, "difficulty", &difficulty);
        json_number(text, "race", &race);
        json_number(text, "regionIndex", &region);
        const int template = template_of_id((uint32_t)settings);
        api->log(self, "operation created (%s): planet %lld, difficulty %lld, race %lld, operationSettingsId %llu = template %d, seed %lld%s",
                 name, planet, difficulty, race, (unsigned long long)(uint32_t)settings, template, (long long)(int32_t)seed,
                 armed.enabled && planet == armed.preset.planet && armed.template_id && template == (int)armed.preset.template_index ? "  [matches template_index]" : "");
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

static void mission_start(HfMod *mod, const HfStateEvent *event, void *user)
{
    (void)mod;
    (void)event;
    (void)user;
    HfSession session = { .role = HF_SESSION_SOLO };
    api->session(self, &session);
    const char *role = session.role == HF_SESSION_JOINER ? "joiner" : session.role == HF_SESSION_HOST ? "host" : "solo";
    uintptr_t board = game ? game_pointer(BoardPointer) : 0;
    unsigned char row[OperationStride];
    if (!board || !read_bytes(board + NativeActive, row, sizeof row)) {
        api->log(self, "mission start (%s): no active operation to read", role);
        return;
    }
    uint16_t planet;
    uint32_t seed, template, hash = 0;
    memcpy(&planet, row + 16, sizeof planet);
    memcpy(&seed, row + 12, sizeof seed);
    memcpy(&template, row + 56, sizeof template);
    read_bytes(definition_hash_address(board, planet), &hash, sizeof hash);
    World world = {0};
    char ids[HOST_OPERATIONS_MAX_EFFECTS * 11 + 1] = "";
    if (read_world(board, planet, &world)) {
        size_t used = 0;
        for (uint32_t i = 0; i < world.effect_count; ++i)
            used += (size_t)snprintf(ids + used, sizeof ids - used, "%s%u", i ? "," : "", world.effects[i]);
    }
    api->log(self, "mission start (%s): planet %u, settingsHash %u%s, template %u, seed %u, difficulty %u, faction %u, effects [%s]",
             role, planet, hash, active_board == board && planet == armed.preset.planet && armed.preset.biome == hash ? " (this mod's)" : "", template, seed, row[32],
             world.faction, ids);
}

static uint32_t last_state = HF_STATE_NONE;

/* The live world is also consumed after a card is accepted. Keep its effective
 * fields available to the host while that operation is active, including load-in. */
static void apply_active_world(uintptr_t board, uint32_t state)
{
    if (!board || !armed.enabled || restore_failed) return; /* host, or a joiner following the same sync code */
    uint32_t planet;
    const bool active = action_active(board, &planet) && planet == armed.preset.planet;
    if (!active) {
        if (state == HF_STATE_SHIP) restore_active_world(board);
        return;
    }
    World world;
    if (active_board == board && active_revision == armed.serial) {
        if (!recheck_active_world) return;
        recheck_active_world = false;
        uint32_t current_hash;
        if (read_bytes(definition_hash_address(board, planet), &current_hash, sizeof current_hash) &&
            current_hash == (armed.preset.biome ? armed.preset.biome : armed.settings_hash) &&
            read_world(board, planet, &world) && world.enabled && world.faction == armed.preset.faction &&
            snapshot_has_effects(&armed.preset, &world)) return;
    }
    restore_active_world(board);
    Overlay changes = {0};
    if (!world_identity(board, planet, armed.settings_hash) || !read_world(board, planet, &world) ||
        stage_world(board, &armed.preset, &world, &changes) || !write_changes(&changes)) {
        if (!active_write_reported) {
            api->log(self, "Could not apply active world for planet %u", planet);
            menu_report(loc(L"目前行動未能套用所選星球設定。", L"The active operation could not use the chosen world settings."));
            active_write_reported = true;
        }
        return;
    }
    active_changes = changes;
    active_board = board;
    active_revision = armed.serial;
    active_write_reported = false;
}

static void on_tick(HfMod *mod, float dt, void *user)
{
    (void)mod;
    (void)user;
    body_wait += dt;
    if (body_wait >= 3.0f) {
        body_wait = 0.0f;
        scan_operation_records();
    }
    const uint32_t state = api->game_state(self);
    if (hook_ready && game) {
        uintptr_t board = game_pointer(BoardPointer);
        apply_active_world(board, state);
    }
    if (state != last_state) {
        if (state == HF_STATE_MISSION) mission_start(self, NULL, NULL);
        last_state = state;
    }
    const bool ship = state == HF_STATE_SHIP;
    if (!ship && was_on_ship) {
        if (could_edit) menu_set_enabled(false);
        menu_hide();
        could_edit = false;
    }
    was_on_ship = ship;
    const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
    const bool pressed = down && !key_was_down;
    key_was_down = down;
    if (!ship) return;

    if (!language_loaded) {
        menu_load_language();
        language_loaded = true;
    }
    if (!hook_ready && !hook_failed) {
        game_thread = GetCurrentThreadId();
        hook_ready = setup_native();
    }
    HfSession session = { .role = HF_SESSION_JOINER };
    api->session(self, &session);
    uintptr_t board = game ? game_pointer(BoardPointer) : 0;
    uint32_t definitions = 0;
    const bool board_ready = board && read_bytes(board + DefinitionCount, &definitions, sizeof definitions) &&
                             definitions && definitions <= CampaignLimit;
    const bool joiner = session.role == HF_SESSION_JOINER;
    /* Joiners use the menu too: a friend pastes the host's sync code and this game applies the same changes. */
    const bool editable = hook_ready && board_ready && !restore_failed;
    if (could_edit != editable) menu_set_enabled(editable);
    could_edit = editable;
    if (last_role != session.role) {
        /* The opened settings stay: as a joiner they keep changing this game the way the host's code does. */
        restore_active_world(board);
        recheck_active_world = true;
        if (joiner) {
            api->log(self, "Now a joiner; %s", armed.enabled ? "the opened settings keep applying to this game" : "nothing opened");
            menu_report(armed.enabled ? tr(T_OP_JOINER) : NULL);
        } else if (last_role == HF_SESSION_JOINER) {
            menu_report(NULL);
        }
        last_role = session.role;
    }
    if (pressed) {
        DWORD foreground_process = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process);
        if (foreground_process == GetCurrentProcessId()) menu_toggle();
    }

    if (!editable || !board) return;
    OperationPreset draft = {0};
    MenuCommand command;
    uint32_t planet;
    const bool active = action_active(board, &planet);
    if (armed.enabled && active && planet == armed.preset.planet) armed.saw_action = true;
    /* The operation ended (finished, abandoned or left mid-mission). The planet stays open, saved or not, until the
     * host withdraws it or the game closes. Back on the ship the board is rebuilt with it: the game's own refresh while
     * loading back may have run before this mod could apply anything. */
    if (armed.enabled && armed.saw_action && !active && state == HF_STATE_SHIP) {
        armed.saw_action = false;
        restore_active_world(board);
        const bool rebuilt = !joiner && refresh(board);
        api->log(self, "Operation on planet %u ended; the planet stays open (%s), board rebuilt: %s",
                 armed.preset.planet, armed.saved ? "saved" : "this session", rebuilt ? "yes" : "no");
        menu_report(tr(T_OP_TEMP_ENDED));
    }
    if (menu_take_request(&command, &draft)) {
        switch (command) {
            case MENU_OPEN_TEMPORARILY:
            case MENU_SAVE_AND_OPEN:
            case MENU_CLOSE_PLANET:
                queued_command = command;
                queued_preset = draft;
                request_queued = true;
                break;
        }
    }
    /* Applied at once, also while an operation is active (e.g. after leaving a mission mid-way): the active operation
     * of the entry planet takes the new world settings at the next tick, and new cards and missions use the rest. */
    if (request_queued) {
        request_queued = false;
        saved_checked = true;
        if (queued_command == MENU_CLOSE_PLANET) {
            armed.enabled = false;
            restore_active_world(board);
            if (!joiner) refresh(board);
            menu_report(menu_clear_saved() ? tr(T_OP_WITHDRAWN) : tr(T_OP_WITHDRAWN_SAVE_FAILED));
        } else {
            menu_report(arm(board, &queued_preset, queued_command == MENU_SAVE_AND_OPEN));
        }
    } else if (!saved_checked) {
        uint32_t count;
        if (read_bytes(board + DefinitionCount, &count, sizeof count) && count && count <= CampaignLimit) {
            saved_checked = true;
            if (menu_load_saved(&draft)) menu_report(arm(board, &draft, true));
        }
    }
}

static void session_end(HfMod *mod, HfEvent event, void *user)
{
    (void)mod;
    (void)event;
    (void)user;
    menu_shutdown();
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi *hf, HfMod *mod)
{
    if (!hf || hf->version < 16 || hf->size < offsetof(HfApi, read_code) + sizeof hf->read_code ||
        strcmp(hf->build_id(), "25480438")) return 1;
    api = hf;
    self = mod;
    api->log(self, "Press F11 on the ship for host operations (test22: in-game planet and operation editor)");
    if (hf->on(mod, HF_EVENT_SESSION_END, session_end, NULL)) return 1;
    if (!overlay_start(hf, mod)) return 1;
    return hf->on_game_thread(mod, on_tick, NULL) ? 0 : 1;
}
