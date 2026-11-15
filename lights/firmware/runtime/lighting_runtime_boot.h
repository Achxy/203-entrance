#ifndef LIGHTING_RUNTIME_BOOT_H
#define LIGHTING_RUNTIME_BOOT_H
#include <stdbool.h>

/* Boot-local availability gate. The platform classifies reset reasons; this
 * state never changes credentials or persists across a restart. Callers hold
 * the runtime mutex and authenticate the release operation before reaching it. */
typedef struct { bool quarantined, retry_used; } lrb_t;

static inline void lrb_init(lrb_t *gate,bool expected_reset)
{gate->quarantined=!expected_reset;gate->retry_used=false;}

static inline bool lrb_release_once(lrb_t *gate)
{
    if(!gate->quarantined||gate->retry_used)return false;
    gate->quarantined=false;gate->retry_used=true;return true;
}

/* Only after an admission race refused before any worker existed. No start was
 * accepted, so an administrator can retry after the conflicting operation. */
static inline bool lrb_cancel_release(lrb_t *gate)
{
    if(gate->quarantined||!gate->retry_used)return false;
    gate->quarantined=true;gate->retry_used=false;return true;
}
#endif
