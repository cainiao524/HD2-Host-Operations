#ifndef HOST_OPERATIONS_EFFECTS_H
#define HOST_OPERATIONS_EFFECTS_H

#include "catalog.h"
#include "menu.h"

static inline bool known_effect_id(uint32_t id)
{
    for (size_t i = 0; i < planet_effect_count; ++i)
        if (planet_effects[i].id == id)
            return true;
    return false;
}

static inline uint32_t effect_position(const uint32_t *ids, uint32_t count, uint32_t id)
{
    for (uint32_t i = 0; i < count; ++i)
        if (ids[i] == id)
            return i;
    return count;
}

static inline bool append_effect_id(uint32_t *ids, uint32_t *count, uint32_t id)
{
    if (effect_position(ids, *count, id) < *count)
        return true;
    if (*count >= HOST_OPERATIONS_MAX_EFFECTS)
        return false;
    ids[(*count)++] = id;
    return true;
}

#endif
