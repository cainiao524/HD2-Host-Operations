#ifndef HOST_OPERATIONS_I18N_H
#define HOST_OPERATIONS_I18N_H

#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

/* The menu's and the reports' text in Traditional Chinese, Simplified Chinese and English. The language is shared by
 * the menu thread and the game thread (reports are translated when they are made). Simplified text is made from the
 * Traditional text at run time (LCMapStringEx plus a few mainland terms). */
enum Language { LANG_ZH = 0, LANG_EN = 1, LANG_ZH_HANS = 2, LANG_COUNT };

enum Text {
    /* window and layout */
    T_NAV_PLANET, T_NAV_TERRAIN, T_NAV_OPERATION, T_NAV_EFFECTS, T_SUM_OPERATION, T_SUM_EFFECTS_NONE,
    T_SUM_EFFECTS_HEAD, T_SUM_MORE,
    T_WINDOW, T_TITLE, T_SHORTCUT, T_LANG_BUTTON, T_TARGET_TITLE, T_LBL_PLANET_SEARCH, T_LBL_PLANET, T_LBL_BIOME_SEARCH,
    T_LBL_BIOME, T_LBL_LANDING, T_ACTION_TITLE, T_LBL_FACTION, T_LBL_DIFFICULTY, T_LBL_TEMPLATE, T_LBL_SEED, T_LBL_REGION,
    T_EFFECT_TITLE, T_LBL_CATEGORY, T_LBL_SEARCH, T_BTN_CLOSE, T_BTN_SAVE, T_BTN_TEMPORARY,
    /* choices */
    T_FACTION_NATIVE, T_FACTION_BUGS, T_FACTION_BOTS, T_FACTION_SQUIDS, T_DIFFICULTY_TABLE, T_TEMPLATE_GAME,
    T_CUE_PLANET, T_CUE_TERRAIN, T_CUE_EFFECT, T_CUE_SEED, T_CUE_REGION,
    /* pinned planets and terrains */
    T_PIN_MARS, T_PIN_DESERT, T_PIN_HIVE, T_PIN_VOID, T_PIN_FACTORY, T_PIN_MAGMA, T_PIN_SUPER_EARTH, T_PIN_NO_OPS,
    T_TER_MARS, T_TER_MARS2, T_TER_CONIFER, T_TER_CONIFER_PLANET, T_TER_MAGMA, T_TER_HIVE, T_TER_VOID, T_TER_HIVE_PLANET,
    T_TER_FACTORY, T_TER_MAGMA_PLANET, T_TER_SUPER_EARTH, T_TER_UNREGISTERED, T_TER_NATIVE, T_TER_CUSTOM,
    T_ALIAS_VOID, T_ALIAS_CONIFER,
    /* landing panel */
    T_LAND_PICK_PLANET, T_LAND_PICK_PLANET_HINT, T_LAND_NO_PLANET, T_LAND_NO_PLANET_HINT, T_LAND_PICK_TERRAIN,
    T_LAND_NO_TERRAIN, T_LAND_PICK_TERRAIN_HINT, T_LAND_NO_TERRAIN_HINT, T_LAND_ENTRY, T_LAND_OWN, T_LAND_CUSTOM,
    T_LAND_DONOR, T_LAND_TEMPLATE_NOTE,
    /* effects */
    T_TIER_ALL, T_TIER_PART, T_TIER_NONE, T_F_ALL, T_F_BUGS, T_F_BOTS, T_F_SQUIDS, T_F_SHARED, T_F_VARIANTS, T_F_SELECTED,
    T_EFFECT_COUNT, T_EFFECT_HINT, T_EFFECT_EMPTY, T_EFFECT_CUSTOM_NAME, T_EFFECT_CUSTOM_CATEGORY, T_EFFECT_UNNAMED,
    T_EFFECT_TOO_MANY,
    /* requests */
    T_ERR_PLANET, T_ERR_DIFFICULTY, T_ERR_REGION, T_ERR_SEED, T_ERR_FACTION, T_ERR_TERRAIN, T_STATUS_READY, T_STATUS_WAIT,
    T_SUBMIT_UNAVAILABLE, T_SUBMIT_PENDING, T_SUBMIT_SENT,
    T_ERR_THREAD, T_ERR_MODULE, T_ERR_REGISTER, T_ERR_LOOP, T_ERR_WINDOW,
    /* reports from the war table (operations.c) */
    T_OP_MAX_EFFECTS, T_OP_TOO_MANY, T_OP_INVALID, T_OP_REGION_FACTION, T_OP_OTHER_CAMPAIGN, T_OP_BUILD_MISMATCH,
    T_OP_HOOK_FAILED, T_OP_NOT_READY, T_OP_CHECK, T_OP_SELECTION, T_OP_PLANET, T_OP_NO_IDENTITY, T_OP_NO_REGION,
    T_OP_FACTION, T_OP_SNAPSHOT, T_OP_SAVE, T_OP_PLANET_N, T_OP_EXTRA_TERRAIN, T_OP_EXTRA_TEMPLATE, T_OP_OPENED,
    T_OP_NOT_CONFIRMED, T_OP_SELECTED, T_OP_NO_CANDIDATE, T_OP_JOINER, T_OP_TEMP_ENDED, T_OP_QUEUED, T_OP_WITHDRAWN,
    T_OP_WITHDRAWN_SAVE_FAILED,
    T_COUNT
};

/* The template names, 0..24, in the current language. */
const wchar_t *template_name(uint32_t index);
/* The current language's text. */
const wchar_t *tr(enum Text id);
/* zh (Traditional) or en in the current language; Simplified is made from zh. The pointers must stay valid. */
const wchar_t *loc(const wchar_t *zh, const wchar_t *en);
/* Simplified Chinese for a Traditional (or mixed) text; cached by pointer, so pass only lasting strings. */
const wchar_t *simplified(const wchar_t *text);
/* The same conversion into a caller's buffer (any text). */
void to_simplified(const wchar_t *text, wchar_t *out, size_t capacity);
enum Language ui_language(void);
void set_ui_language(enum Language language);
/* An English name for a catalogue entry: the plainest English part of its aliases, or NULL. */
bool english_alias(const char *aliases, wchar_t *out, size_t capacity);
/* An English name for a catalogue category (the catalogue's are Simplified Chinese), or NULL. */
const wchar_t *english_category(const char *category);

#endif
