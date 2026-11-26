/* Offline host-board fixture with the captured layout for build 25480438. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "../src/operations.c"

static uintptr_t board;
enum { BoardBytes = CampaignSnapshot + 0x78ee8 };
/* ID, enabled, faction, minimum and maximum difficulty in the pinned build. */
static const uint32_t headers[TemplateCount][5] = {
    {91950620u, 1u, 2u, 1u, 11u},
    {2350081063u, 1u, 2u, 1u, 11u},
    {1991153346u, 1u, 2u, 1u, 11u},
    {3766033139u, 1u, 2u, 1u, 11u},
    {82127679u, 1u, 2u, 1u, 11u},
    {3724394067u, 1u, 2u, 1u, 11u},
    {1536944888u, 1u, 2u, 1u, 11u},
    {3245753142u, 1u, 2u, 1u, 1u},
    {538077316u, 1u, 3u, 1u, 11u},
    {2807074392u, 1u, 3u, 1u, 11u},
    {3204348594u, 1u, 3u, 1u, 11u},
    {2570368464u, 1u, 3u, 1u, 11u},
    {4255408343u, 1u, 3u, 1u, 11u},
    {438618731u, 1u, 3u, 1u, 11u},
    {3866136882u, 1u, 3u, 1u, 11u},
    {2796992372u, 1u, 4u, 1u, 11u},
    {271746735u, 1u, 4u, 1u, 11u},
    {2904619323u, 1u, 4u, 1u, 11u},
    {2452579076u, 1u, 4u, 1u, 11u},
    {152966721u, 0u, 4u, 1u, 11u},
    {3194790369u, 1u, 4u, 1u, 11u},
    {878895250u, 1u, 4u, 1u, 11u},
    {1488366454u, 1u, 4u, 1u, 11u},
    {3120863352u, 1u, 4u, 1u, 11u},
    {1334938443u, 1u, 4u, 1u, 11u}
};
static unsigned calls, writes, template_writes;
static bool fail_generation;
static uintptr_t fail_write_at;
static uint32_t role = HF_SESSION_HOST;
static const uint32_t original_id = 2350081063u;

static bool in_board(uintptr_t address, size_t n)
{
    return address >= board && n <= BoardBytes && address - board <= BoardBytes - n;
}

static int fixture_read(uintptr_t address, void *dest, size_t n)
{
    if (address == game + BoardPointer && n == sizeof board) memcpy(dest, &board, n);
    else if (address >= game + TemplateTable && address < game + TemplateTable + TemplateCount * TemplateStride) {
        size_t offset = address - (game + TemplateTable);
        if (offset % TemplateStride || n > sizeof headers[0]) return 1;
        memcpy(dest, headers[offset / TemplateStride], n);
    } else if (in_board(address, n)) memcpy(dest, (void *)address, n);
    else return 1;
    return 0;
}

static int fixture_write(uintptr_t address, const void *data, size_t n)
{
    if (address == fail_write_at) return 1;
    if (!in_board(address, n)) { ++template_writes; return 1; }
    ++writes;
    memcpy((void *)address, data, n);
    return 0;
}

static uint32_t get32(uintptr_t address)
{
    uint32_t value;
    assert(fixture_read(address, &value, sizeof value) == 0);
    return value;
}

static void put32(uintptr_t address, uint32_t value)
{
    assert(fixture_write(address, &value, sizeof value) == 0);
}

static void log_discard(HfMod *mod, const char *format, ...) { (void)mod; (void)format; }
static uint32_t ship(HfMod *mod) { (void)mod; return HF_STATE_SHIP; }
static void session(HfMod *mod, HfSession *out) { (void)mod; out->role = role; }
static bool vanilla_build(uintptr_t row)
{
    ++calls;
    const uint32_t id = get32(row + 8);
    uint32_t index = 1;
    if (id != original_id && id) {
        for (index = 0; index < TemplateCount && headers[index][0] != id; ++index) {}
        if (index == TemplateCount || fail_generation) return false;
    }
    put32(row + 56, index);
    *(uint8_t *)(row + 52) = 1;
    return true;
}

static uintptr_t vanilla_refresh(uintptr_t b)
{
    memcpy((void *)(b + CampaignSnapshot), (void *)b, 0x78ee8);
    put32(b + NativeOperations + 8, original_id);
    build_row_detour(b + NativeOperations);
    return 0;
}

static void card(uintptr_t at, uint16_t planet, uint8_t difficulty)
{
    memset((void *)at, 0, OperationStride);
    memcpy((void *)(at + 16), &planet, sizeof planet);
    *(uint8_t *)(at + 32) = difficulty;
    put32(at + 8, original_id);
    put32(at + 12, 0x12345678);
    put32(at + 36, 2);
}

bool overlay_start(const HfApi *hf, HfMod *mod) { (void)hf; (void)mod; return true; }
void overlay_set_visible(bool visible) { (void)visible; }
void overlay_stop(void) {}

static void check_saved_preset(void)
{
    wchar_t scratch[MAX_PATH], root[MAX_PATH], folder[MAX_PATH + 64];
    assert(GetTempPathW(MAX_PATH, scratch) > 0 && GetTempFileNameW(scratch,L"HCO",0,root));
    assert(DeleteFileW(root) && CreateDirectoryW(root,NULL));
    assert(SetEnvironmentVariableW(L"LOCALAPPDATA",root));
    OperationPreset old = {.planet=1, .region=2, .difficulty=10, .seed=900, .operation_template=42,
                           .biome=2100206204, .template_index=13, .effect_count=1, .effects={1245}};
    assert(menu_save_preset(&old));
    OperationPreset loaded;
    assert(menu_load_saved(&loaded) && loaded.planet == old.planet && loaded.template_index == 13 &&
           loaded.biome == old.biome && loaded.effect_count == 1 && loaded.effects[0] == 1245);
    assert(loaded.region == HOST_OPERATIONS_NO_INDEX && !loaded.difficulty && !loaded.operation_template && !loaded.seed);
    menu_set_enabled(true);
    assert(host_ui_submit(MENU_OPEN_TEMPORARILY,&loaded));
    assert(!host_ui_submit(MENU_SAVE_AND_OPEN,&loaded));
    MenuCommand cmd;
    OperationPreset taken;
    assert(menu_take_request(&cmd,&taken) && cmd == MENU_OPEN_TEMPORARILY && !memcmp(&taken,&loaded,sizeof taken));
    menu_set_enabled(false);
    assert(!host_ui_submit(MENU_OPEN_TEMPORARILY,&loaded));
    assert(menu_clear_saved() && !menu_load_saved(&loaded));
    swprintf(folder,ARRAYSIZE(folder),L"%ls\\HellForge\\HostOperationsTest",root);
    assert(RemoveDirectoryW(folder));
    swprintf(folder,ARRAYSIZE(folder),L"%ls\\HellForge",root);
    assert(RemoveDirectoryW(folder) && RemoveDirectoryW(root));
}

int main(void)
{
    static const HfApi fixture = {.log=log_discard, .game_state=ship, .session=session, .read=fixture_read, .write=fixture_write};
    api = &fixture;
    board = (uintptr_t)calloc(1, BoardBytes);
    assert(board);
    /* arm() dispatches the native entry and then verifies the snapshot it produced. */
    game = (uintptr_t)refresh_detour - RefreshBoard;
    game_thread = GetCurrentThreadId();
    original_build = (void *)vanilla_build;
    original_refresh = (void *)vanilla_refresh;
    hook_ready = true;
    const uint32_t planet = 1, own = find_planet(1)->settings_hash;
    const uintptr_t definition = definition_hash_address(board, planet), row = board + NativeOperations;
    const uintptr_t world = board + planet * WorldStride;
    put32(board + DefinitionCount, 274);
    put32(definition, own);
    put32(world + WorldFaction, 2);
    put32(world + WorldEffects, 1243);
    put32(world + WorldEffectCount, 1);
    const CampaignRow existing[] = {{200, 3, 1, 4, 3}, {201, 1, 1, 2, 4}, {202, 1, 5, 1, 2}};
    memcpy((void *)(board + CampaignRows), existing, sizeof existing);
    put32(board + CampaignCount, 3);
    OperationPreset p = {.planet=planet, .region=HOST_OPERATIONS_NO_INDEX, .template_index=13,
        .difficulty=11, .biome=2100206204, .effect_count=2, .effects={1243,1245}};
    for (unsigned i = 0; i < TemplateCount; ++i) {
        uint8_t difficulty = i == 7 ? 1 : 11;
        p.template_index = i;
        card(row, planet, difficulty);
        arm(board, &p, false);
        assert(armed.enabled && armed.preset.template_index == i && armed.row_verified);
        assert(get32(row + 8) == headers[i][0] && get32(row + 56) == i && get32(row + 12) == 0x12345678);
        assert(get32(row + 36) == headers[i][2]);
        assert(snapshot_applied && get32(definition + CampaignSnapshot) == p.biome && get32(definition) == own);
        assert(get32(world + CampaignSnapshot + WorldFaction) == headers[i][2]);
        assert(get32(world + CampaignSnapshot + WorldEffectCount) == 2 && get32(world + WorldEffectCount) == 1);
        assert(get32(board + CampaignSnapshot + CampaignCount) == 3);
        assert(!memcmp((void *)(board + CampaignRows + CampaignSnapshot), existing, sizeof existing));
        assert(!memcmp((void *)(board + CampaignRows), existing, sizeof existing));
    }
    card(row, 5, 11);
    assert(build_row_detour(row) && get32(row + 8) == original_id);
    card(board + NativeActive, planet, 11);
    assert(build_row_detour(board + NativeActive) && get32(board + NativeActive + 8) == original_id);
    card(row, planet, 11);
    role = HF_SESSION_JOINER;
    assert(build_row_detour(row) && get32(row + 8) == original_id);
    role = HF_SESSION_HOST;
    p.template_index = 7;
    arm(board, &p, false);
    assert(get32(row + 8) == original_id && !armed.row_verified);
    card(row, planet, 1);
    fail_generation = true;
    unsigned before = calls;
    assert(build_row_detour(row) && calls == before + 2 && get32(row + 8) == original_id && armed.row_failed);
    fail_generation = false;
    p.faction = 4;
    p.template_index = 13;
    arm(board, &p, false);
    assert(armed.preset.template_index == 7);
    p.faction = 0;
    put32(board + CampaignCount, 0);
    arm(board, &p, false);
    assert(armed.preset.template_index == 13 && get32(board + CampaignSnapshot + CampaignCount) == 1);
    assert(get32(board + CampaignCount) == 0);
    fail_write_at = world + WorldFaction;
    p.template_index = 19;
    arm(board, &p, false);
    assert(armed.preset.template_index == 13 && get32(definition) == own && !get32(board + CampaignCount));
    fail_write_at = 0;
    apply_active_world(board, HF_STATE_SHIP);
    assert(get32(definition) == p.biome && get32(world + WorldFaction) == 3 && get32(world + WorldEffectCount) == 2);
    put32(world + WorldFaction, 4); /* A new native war update reached the active planet. */
    recheck_active_world = true;
    apply_active_world(board, HF_STATE_MISSION);
    assert(get32(world + WorldFaction) == 3);
    *(uint8_t *)(board + NativeActive + 52) = 0;
    apply_active_world(board, HF_STATE_SHIP);
    assert(get32(definition) == own && !active_board && get32(world + WorldFaction) == 4 && get32(world + WorldEffectCount) == 1);
    p.template_index = HOST_OPERATIONS_NO_TEMPLATE;
    arm(board, &p, false);
    assert(armed.template_id == 0 && get32(row + 8) == original_id);
    assert(writes && !template_writes);
    free((void *)board);
    check_saved_preset();
    puts("PASS: 25 templates, target isolation, hidden template, difficulty, factions, seeds, terrain/effects, active campaigns, rollback, joiner, saved preset migration, UI command handoff");
}
