#ifndef LIGHTING_RUNTIME_PROVISION_H
#define LIGHTING_RUNTIME_PROVISION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LPP_TTL_MS UINT64_C(30000)
#define LPP_ATTEMPT_MS UINT64_C(1000)
#define LPP_PROFILE_BYTES 800u

typedef enum {
    LPP_IDLE=0, LPP_QUEUED, LPP_ADMITTED, LPP_COMMITTING,
    LPP_COMMITTED, LPP_FAILED, LPP_EXPIRED, LPP_CANCELLED
} lpp_phase_t;
typedef struct {
    uint32_t generation;
    lpp_phase_t phase;
    uint64_t accepted_ms, deadline_ms, next_attempt_ms;
    bool cancel_requested;
    uint8_t profiles[LPP_PROFILE_BYTES];
} lpp_job_t;

bool lpp_active(const lpp_job_t *job);
/* One fixed authorization interval; requeue/duplicate calls never extend it.
 * Generation is unique within a boot and exhaustion fails closed. */
bool lpp_enqueue(lpp_job_t *job,uint64_t now_ms,const void *profiles,size_t size);
bool lpp_expire(lpp_job_t *job,uint64_t now_ms);
bool lpp_due(const lpp_job_t *job,uint32_t generation,uint64_t now_ms);
bool lpp_refused(lpp_job_t *job,uint32_t generation,uint64_t now_ms);
bool lpp_admit(lpp_job_t *job,uint32_t generation,uint64_t now_ms);
/* Commit admission is the last cancellable boundary. A transaction already
 * admitted must report its durable/unknown outcome even after the deadline. */
bool lpp_begin_commit(lpp_job_t *job,uint32_t generation,uint64_t now_ms,
                      void *private_profiles,size_t size);
bool lpp_complete(lpp_job_t *job,uint32_t generation,bool committed);
bool lpp_cancel(lpp_job_t *job);
const char *lpp_phase_name(lpp_phase_t phase);
#endif
