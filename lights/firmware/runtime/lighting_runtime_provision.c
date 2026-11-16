#include "lighting_runtime_provision.h"
#include <string.h>

static void wipe(uint8_t *bytes,size_t size)
{volatile uint8_t *p=bytes;while(size--)*p++=0;}

bool lpp_active(const lpp_job_t *j)
{return j&&(j->phase==LPP_QUEUED||j->phase==LPP_ADMITTED||j->phase==LPP_COMMITTING);}
bool lpp_enqueue(lpp_job_t *j,uint64_t now,const void *profiles,size_t size)
{
    if(!j||!profiles||size!=LPP_PROFILE_BYTES||lpp_active(j)||
       j->generation==UINT32_MAX||now>UINT64_MAX-LPP_TTL_MS||
       (j->generation&&now<j->accepted_ms))return false;
    ++j->generation;j->phase=LPP_QUEUED;j->accepted_ms=now;
    j->deadline_ms=now+LPP_TTL_MS;j->next_attempt_ms=now+LPP_ATTEMPT_MS;
    j->cancel_requested=false;memcpy(j->profiles,profiles,LPP_PROFILE_BYTES);return true;
}
bool lpp_expire(lpp_job_t *j,uint64_t now)
{
    if(!j||(j->phase!=LPP_QUEUED&&j->phase!=LPP_ADMITTED)||
       (now>=j->accepted_ms&&now<j->deadline_ms))return false;
    j->phase=LPP_EXPIRED;wipe(j->profiles,sizeof(j->profiles));return true;
}
bool lpp_due(const lpp_job_t *j,uint32_t generation,uint64_t now)
{
    return j&&j->generation==generation&&j->phase==LPP_QUEUED&&
           now>=j->accepted_ms&&now>=j->next_attempt_ms&&now<j->deadline_ms;
}
bool lpp_refused(lpp_job_t *j,uint32_t generation,uint64_t now)
{
    if(!lpp_due(j,generation,now))return false;
    j->next_attempt_ms=j->deadline_ms-now>LPP_ATTEMPT_MS?now+LPP_ATTEMPT_MS:j->deadline_ms;
    return true;
}
bool lpp_admit(lpp_job_t *j,uint32_t generation,uint64_t now)
{
    if(!lpp_due(j,generation,now))return false;
    j->phase=LPP_ADMITTED;return true;
}
bool lpp_begin_commit(lpp_job_t *j,uint32_t generation,uint64_t now,void *profiles,size_t size)
{
    if(!j||!profiles||size!=LPP_PROFILE_BYTES||j->generation!=generation||
       j->phase!=LPP_ADMITTED||j->cancel_requested||
       now<j->accepted_ms||now>=j->deadline_ms)return false;
    memcpy(profiles,j->profiles,LPP_PROFILE_BYTES);wipe(j->profiles,sizeof(j->profiles));
    j->phase=LPP_COMMITTING;return true;
}
bool lpp_complete(lpp_job_t *j,uint32_t generation,bool committed)
{
    if(!j||j->generation!=generation||!lpp_active(j)||(committed&&j->phase!=LPP_COMMITTING))return false;
    j->phase=committed?LPP_COMMITTED:LPP_FAILED;wipe(j->profiles,sizeof(j->profiles));return true;
}
bool lpp_cancel(lpp_job_t *j)
{
    if(!lpp_active(j))return false;
    j->cancel_requested=true;
    if(j->phase==LPP_COMMITTING)return false;
    j->phase=LPP_CANCELLED;wipe(j->profiles,sizeof(j->profiles));return true;
}
const char *lpp_phase_name(lpp_phase_t phase)
{
    static const char *const names[]={"idle","queued","admitted","committing","committed","failed","expired","cancelled"};
    return (unsigned)phase<sizeof(names)/sizeof(names[0])?names[phase]:"unknown";
}
