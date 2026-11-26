/* Sync code round trip: build with zig cc together with src/menu.c, catalog_data.c, mission_catalog.c, i18n.c. */
#include <stdbool.h>
void overlay_set_visible(bool v) { (void)v; }
void overlay_stop(void) {}
/* Round trip of the sync code: preset -> code -> preset, plus a corrupted code. */
#include <stdio.h>
#include <string.h>
#include "menu.h"

int main(void)
{
    OperationPreset p = {0};
    p.planet = 121; p.region = UINT32_MAX; p.biome = 2871915822u; p.faction = 2; p.template_index = 1;
    p.missions[0] = 1440754583u; p.missions[1] = 0; p.missions[2] = 1910673334u;
    const uint32_t effects[] = {1401, 1400, 1243, 1245, 1303, 1310};
    p.effect_count = 6;
    memcpy(p.effects, effects, sizeof effects);
    char code[256];
    if (!menu_preset_code(&p, code, sizeof code)) { puts("encode failed"); return 1; }
    printf("code %s (%zu chars)\n", code, strlen(code));
    OperationPreset q;
    if (!menu_parse_code(code, &q)) { puts("decode failed"); return 1; }
    int same = q.planet == p.planet && q.biome == p.biome && q.faction == p.faction && q.template_index == p.template_index &&
               !memcmp(q.missions, p.missions, sizeof p.missions) && q.effect_count == p.effect_count &&
               !memcmp(q.effects, p.effects, sizeof effects);
    printf("round trip %s\n", same ? "same" : "DIFFERENT");
    char lower[256];
    for (size_t i = 0; i <= strlen(code); ++i) lower[i] = (char)(code[i] >= 'A' && code[i] <= 'Z' ? code[i] + 32 : code[i]);
    printf("lowercase + spaces %s\n", menu_parse_code(lower, &q) ? "ok" : "FAILED");
    code[10] = code[10] == 'A' ? 'B' : 'A';
    printf("corrupted %s\n", menu_parse_code(code, &q) ? "ACCEPTED (bad)" : "rejected");
    p.template_index = UINT32_MAX; p.effect_count = 0; memset(p.missions, 0, sizeof p.missions);
    if (!menu_preset_code(&p, code, sizeof code) || !menu_parse_code(code, &q) || q.template_index != UINT32_MAX) {
        puts("game-own template round trip FAILED"); return 1;
    }
    printf("minimal code %s\n", code);
    return same ? 0 : 1;
}
