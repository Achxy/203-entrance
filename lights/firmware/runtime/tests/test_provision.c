#include "lighting_runtime_provision.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool zero(const uint8_t *bytes,size_t size)
{for(size_t i=0;i<size;++i)if(bytes[i])return false;return true;}
static void fixture(uint8_t profiles[LPP_PROFILE_BYTES])
{for(size_t i=0;i<LPP_PROFILE_BYTES;++i)profiles[i]=(uint8_t)(i%251u+1u);}

int main(void)
{
    _Static_assert(sizeof(lpp_job_t)<=864,"provision job exceeded bounded storage");
    uint8_t profiles[LPP_PROFILE_BYTES],expected[LPP_PROFILE_BYTES],private_copy[LPP_PROFILE_BYTES];
    fixture(profiles);memcpy(expected,profiles,sizeof(expected));
    lpp_job_t job={0};
    assert(!lpp_active(&job)&&!lpp_due(&job,0,0));
    assert(!lpp_enqueue(&job,100,NULL,sizeof(profiles)));
    assert(!lpp_enqueue(&job,100,profiles,sizeof(profiles)-1));
    assert(lpp_enqueue(&job,100,profiles,sizeof(profiles)));
    uint32_t first=job.generation;uint64_t deadline=job.deadline_ms;
    assert(first==1&&deadline==30100&&job.phase==LPP_QUEUED);
    memset(profiles,0,sizeof(profiles)); /* The HTTP adapter may wipe its own input. */
    assert(!memcmp(job.profiles,expected,sizeof(expected)));
    assert(!lpp_enqueue(&job,900,profiles,sizeof(profiles))&&job.deadline_ms==deadline);
    assert(!lpp_due(&job,first,1099)&&!lpp_admit(&job,first,1099));
    assert(lpp_due(&job,first,1100));

    /* A live TLS connection can deny each resource admission without renewing
     * authorization or duplicating a worker. The final retry is never late. */
    for(uint64_t now=1100;now<deadline;now+=1000) {
        assert(lpp_refused(&job,first,now));
        assert(job.deadline_ms==deadline&&job.phase==LPP_QUEUED);
        assert(!lpp_refused(&job,first,now));
        assert(!lpp_due(&job,first,now+999));
    }
    assert(!lpp_due(&job,first,deadline));
    assert(lpp_expire(&job,deadline)&&job.phase==LPP_EXPIRED);
    assert(zero(job.profiles,sizeof(job.profiles))&&!lpp_active(&job));
    assert(!lpp_complete(&job,first,true)&&!lpp_admit(&job,first,deadline));

    /* ABA: a stale poll/worker from the first request cannot claim or complete
     * the second request, even when a new profile happens to be identical. */
    fixture(profiles);
    assert(lpp_enqueue(&job,40000,profiles,sizeof(profiles)));
    uint32_t second=job.generation;assert(second==2);
    assert(!lpp_admit(&job,first,41000));
    assert(!lpp_complete(&job,first,false));
    assert(lpp_admit(&job,second,41000)&&!lpp_admit(&job,second,41000));
    assert(!lpp_expire(&job,69999)); /* Earlier preflight was just inside the deadline. */
    uint64_t claim_now=70000; /* Storage load/CPU scheduling crossed the boundary. */
    assert(lpp_expire(&job,claim_now));
    memset(private_copy,0,sizeof(private_copy));
    assert(!lpp_begin_commit(&job,second,claim_now,private_copy,sizeof(private_copy)));
    assert(job.phase==LPP_EXPIRED); /* Classify cancellation, never storage corruption. */
    assert(zero(private_copy,sizeof(private_copy))&&zero(job.profiles,sizeof(job.profiles)));

    /* Queued and admitted cancellation wipe only the shared slot. */
    for(unsigned admitted=0;admitted<2;++admitted) {
        assert(lpp_enqueue(&job,80000+admitted*40000,profiles,sizeof(profiles)));
        if(admitted)assert(lpp_admit(&job,job.generation,121000));
        assert(lpp_cancel(&job)&&job.phase==LPP_CANCELLED);
        assert(zero(job.profiles,sizeof(job.profiles)));
        assert(!lpp_cancel(&job)&&!lpp_complete(&job,job.generation,true));
    }

    /* Commit owns an immutable private copy. Cancellation/expiry cannot erase
     * the worker's input or claim that a transaction already started vanished. */
    assert(lpp_enqueue(&job,160000,profiles,sizeof(profiles)));
    assert(lpp_admit(&job,job.generation,161000));
    assert(!lpp_begin_commit(&job,job.generation,161000,private_copy,sizeof(private_copy)-1));
    assert(lpp_begin_commit(&job,job.generation,189999,private_copy,sizeof(private_copy)));
    assert(!memcmp(private_copy,expected,sizeof(expected))&&zero(job.profiles,sizeof(job.profiles)));
    assert(!lpp_expire(&job,190000)&&!lpp_cancel(&job));
    assert(job.phase==LPP_COMMITTING&&job.cancel_requested);
    assert(!memcmp(private_copy,expected,sizeof(expected)));
    assert(lpp_complete(&job,job.generation,true)&&job.phase==LPP_COMMITTED);
    assert(!lpp_complete(&job,job.generation,false));
    memset(private_copy,0,sizeof(private_copy)); /* Worker cleanup after COW returns. */

    /* Allocation failure before commit and uncertain NVS failure after commit
     * both become terminal; neither can auto-requeue its accepted credential. */
    for(unsigned committing=0;committing<2;++committing) {
        uint64_t now=200000+committing*40000;
        assert(lpp_enqueue(&job,now,profiles,sizeof(profiles)));
        assert(lpp_admit(&job,job.generation,now+1000));
        if(committing)assert(lpp_begin_commit(&job,job.generation,now+1000,private_copy,sizeof(private_copy)));
        assert(lpp_complete(&job,job.generation,false)&&job.phase==LPP_FAILED);
        assert(zero(job.profiles,sizeof(job.profiles))&&!lpp_due(&job,job.generation,now+2000));
        memset(private_copy,0,sizeof(private_copy));
    }
    assert(lpp_enqueue(&job,300000,profiles,sizeof(profiles)));
    assert(lpp_expire(&job,299999)&&zero(job.profiles,sizeof(job.profiles)));
    assert(!lpp_enqueue(&job,299999,profiles,sizeof(profiles)));
    job.generation=UINT32_MAX;
    assert(!lpp_enqueue(&job,400000,profiles,sizeof(profiles)));
    job=(lpp_job_t){0}; /* Reset drops RAM-only authorization. */
    assert(!lpp_active(&job)&&zero(job.profiles,sizeof(job.profiles)));
    assert(!lpp_enqueue(&job,UINT64_MAX-LPP_TTL_MS+1,profiles,sizeof(profiles)));
    assert(lpp_enqueue(&job,UINT64_MAX-LPP_TTL_MS,profiles,sizeof(profiles)));
    assert(lpp_expire(&job,UINT64_MAX));
    printf("PASS provision TTL, stale generation, immutable claim, cancellation, terminal failures and wipe; job=%zu bytes\n",sizeof(job));
}
