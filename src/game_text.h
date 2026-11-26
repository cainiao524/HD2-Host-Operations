#ifndef HOST_OPERATIONS_GAME_TEXT_H
#define HOST_OPERATIONS_GAME_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* The game's own text (UTF-8), indexed like enum Language: [0] Traditional Chinese, [1] English, [2] Simplified
 * Chinese. Empty strings where the game has no text for the entry. */
typedef struct GameText {
    const char *text[3];
} GameText;

typedef struct NamedText {
    const char *english; /* the catalogue's English name */
    GameText name;
} NamedText;

typedef struct IdText {
    uint32_t id;
    GameText name;
} IdText;

extern const GameText template_text[25];   /* operation template rows 0-24 */
extern const GameText faction_text[5];     /* by faction: 2 Terminids, 3 Automatons, 4 Illuminate */
extern const GameText mission_text[162];   /* mission table rows */
extern const NamedText planet_text[];      /* planet and terrain names */
extern const size_t planet_text_count;
extern const IdText effect_text[];         /* effects and variant groups, by first ID */
extern const size_t effect_text_count;

#endif
