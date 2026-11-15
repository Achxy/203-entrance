#include "lighting_runtime_recovery.h"

void lrr_failure(lrr_t *r,uint64_t now)
{
    if(!r)return;
    uint64_t delay=r->failures>=4?UINT64_C(300000):UINT64_C(30000)<<r->failures;
    if(r->failures<UINT32_MAX)++r->failures;
    /* A clock overflow must stop retries, never turn into an immediate loop. */
    r->pending=now<=UINT64_MAX-delay;
    r->next_ms=r->pending?now+delay:0;
}
void lrr_success(lrr_t *r)
{
    if(!r)return;
    r->pending=false;r->next_ms=0;r->failures=0;
}
bool lrr_due(const lrr_t *r,uint64_t now,bool eligible)
{return r&&eligible&&r->pending&&now>=r->next_ms;}
bool lrr_take(lrr_t *r,uint64_t now,bool eligible)
{
    if(!lrr_due(r,now,eligible))return false;
    r->pending=false;r->next_ms=0;
    if(r->attempts<UINT32_MAX)++r->attempts;
    return true;
}
