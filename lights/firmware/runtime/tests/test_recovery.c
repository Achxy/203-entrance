#include "lighting_runtime_recovery.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    lrr_t r={0};uint64_t now=100;
    assert(!lrr_due(&r,now,true));
    const uint64_t delays[]={30000,60000,120000,240000,300000,300000};
    for(unsigned i=0;i<sizeof(delays)/sizeof(delays[0]);++i) {
        lrr_failure(&r,now);
        assert(r.pending&&r.next_ms==now+delays[i]);
        assert(!lrr_due(&r,r.next_ms-1,true));
        assert(!lrr_take(&r,r.next_ms,false)); /* A paused/storage admission gate cannot consume a retry. */
        assert(r.pending&&r.attempts==i);
        now=r.next_ms;
        assert(lrr_take(&r,now,true));
        assert(!lrr_take(&r,now,true)); /* Exactly one attempt, even if polled repeatedly. */
        assert(r.attempts==i+1);
    }
    lrr_success(&r);assert(!r.pending&&r.failures==0&&r.attempts==6);
    lrr_failure(&r,now);assert(r.next_ms==now+30000);
    /* A long quiesce leaves one due attempt, not a catch-up burst. */
    assert(!lrr_take(&r,now+3600000,false));
    assert(lrr_take(&r,now+3600000,true));assert(!lrr_take(&r,now+3600000,true));
    lrr_failure(&r,UINT64_MAX-1);assert(!r.pending&&!lrr_due(&r,UINT64_MAX,true));
    r.failures=UINT32_MAX;r.attempts=UINT32_MAX;lrr_failure(&r,0);
    assert(r.failures==UINT32_MAX&&r.next_ms==300000);
    assert(lrr_take(&r,300000,true)&&r.attempts==UINT32_MAX);
    puts("PASS recovery backoff, admission gates, single attempt, reset and overflow bounds");
}
