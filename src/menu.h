#ifndef HOST_OPERATIONS_MENU_H
#define HOST_OPERATIONS_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HOST_OPERATIONS_MAX_EFFECTS 32
#define HOST_OPERATIONS_TEMPLATES 25u
#define HOST_OPERATIONS_NO_TEMPLATE UINT32_MAX
#define HOST_OPERATIONS_MISSION_SLOTS 3u

/* HellForge's own /WarSeason/GalacticWarEffects (its embedded galactic-war-effects.json) defines only these, so they
 * are the effects a joiner without mods can name; others still reach joiners as IDs but show as a question mark. */
static inline bool joiner_knows_effect(uint32_t id)
{
    static const uint32_t ids[] = {1202, 1203, 1243, 1244, 1245, 1248, 1249, 1303, 1306, 1307, 1308, 1309,
                                   1310, 1360, 1361, 1377, 1378, 1379, 1380, 1400, 1401, 1402, 1403, 1413};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; ++i)
        if (ids[i] == id) return true;
    return false;
}

typedef struct OperationPreset {
    uint32_t planet;
    uint32_t region;
    uint32_t difficulty;
    uint32_t operation_template;
    /* The settingsHash the planet takes while it is open (any planet's or template's); 0 keeps its own. */
    uint32_t biome;
    uint32_t faction;
    /* Operation template 0..24 written into the planet's native operations, or HOST_OPERATIONS_NO_TEMPLATE. */
    uint32_t template_index;
    /* Operation seed written with it; 0 keeps the game's. */
    uint32_t seed;
    /* Unique native planet-effect IDs, including every selected member of a variant. */
    uint32_t effect_count;
    uint32_t effects[HOST_OPERATIONS_MAX_EFFECTS];
    /* Missions 1-3 of the entry planet's operations, by mission ID (0: the template's own draw). Only the host's game
     * uses them: joiners without this mod derive the missions from the template and the seed. */
    uint32_t missions[HOST_OPERATIONS_MISSION_SLOTS];
} OperationPreset;

typedef enum MenuCommand {
    MENU_OPEN_TEMPORARILY,
    MENU_SAVE_AND_OPEN,
    MENU_CLOSE_PLANET
} MenuCommand;

/* All calls made by the mod return quickly and may run on the game thread. */
bool menu_toggle(void);
void menu_hide(void);
void menu_shutdown(void);
void menu_load_language(void);
void menu_save_language(void);
void host_ui_status(wchar_t *status, size_t capacity, bool *editable, bool *pending);
bool host_ui_submit(MenuCommand command, const OperationPreset *preset);
void host_ui_closed(void);
bool menu_take_request(MenuCommand *command, OperationPreset *preset);
void menu_report(const wchar_t *status);
void menu_set_enabled(bool enabled);
bool menu_load_saved(OperationPreset *preset);
bool menu_save_preset(const OperationPreset *preset);
bool menu_clear_saved(void);
/* Sync code: the whole preset as text ("HF1-" + Crockford Base32, CRC-16 checked) for friends with this mod. */
bool menu_preset_code(const OperationPreset *preset, char *out, size_t capacity);
bool menu_parse_code(const char *text, OperationPreset *preset);

#endif
