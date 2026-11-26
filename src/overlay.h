#ifndef HOST_OPS_OVERLAY_H
#define HOST_OPS_OVERLAY_H

#include <stdbool.h>
#include "hellforge_mod.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Success means the SDK accepted the game-thread installer. */
bool overlay_start(const HfApi *api, HfMod *mod);
void overlay_set_visible(bool visible);
/* Terminal for this session. Safe in HF_EVENT_SESSION_END. */
void overlay_stop(void);

#ifdef __cplusplus
}
#endif
#endif
