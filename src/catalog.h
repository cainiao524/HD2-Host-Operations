#ifndef HOST_OPERATIONS_CATALOG_H
#define HOST_OPERATIONS_CATALOG_H

#include <stddef.h>
#include <stdint.h>

#define HOST_OPERATIONS_NO_INDEX UINT32_MAX

typedef struct PlanetEntry {
    uint32_t index;
    uint32_t settings_hash;
    uint32_t original_faction;
    const char *name;
    const char *region;
} PlanetEntry;

typedef struct BiomeEntry {
    uint32_t settings_hash;
    uint32_t original_planet;
    const char *name;
    const char *setup;
    const char *region;
} BiomeEntry;

typedef struct VariantEntry {
    uint32_t ids[3];
    uint32_t faction; /* Zero for effects shared by all factions. */
    const char *name;
    const char *aliases;
} VariantEntry;

typedef struct PlanetEffectEntry {
    uint32_t id;
    const char *name;
    const char *category;
    const char *aliases;
} PlanetEffectEntry;

/* A row of the game's mission table (game.dll+0x3773420): what an operation template may list. */
typedef struct MissionEntry {
    uint32_t id;
    uint32_t row;
    uint32_t faction;
    uint32_t mode;       /* 1 normal, 2 horde, 3 defense, 4 opportunity, 5 tutorial, 6 blitz, 7 gloom expedition, 8 commando */
    uint32_t enabled;
    uint32_t min_difficulty, max_difficulty;
    uint32_t used_by;    /* bit n: operation template n lists it */
    const char *name;
} MissionEntry;

static inline uint32_t planet_default_faction(const PlanetEntry *planet)
{
    if (planet->original_faction >= 2 && planet->original_faction <= 4)
        return planet->original_faction;
    return planet->index == 227 ? 4u : 0u; /* Seasse's original Void content uses Illuminate. */
}

extern const PlanetEntry planets[];
extern const size_t planet_count;
extern const BiomeEntry biomes[];
extern const size_t biome_count;
extern const VariantEntry variants[];
extern const size_t variant_count;
extern const PlanetEffectEntry planet_effects[];
extern const size_t planet_effect_count;
extern const MissionEntry mission_catalog[];
extern const size_t mission_catalog_count;

#endif
