#include "lighting_runtime_queue.h"
#include <string.h>

static lrq_slot_t *find(lrq_t *q,const uint8_t route[16])
{
    for(unsigned i=0;i<LRQ_SLOTS;++i)
        if(q->slots[i].used && !memcmp(q->slots[i].work.route,route,16)) return &q->slots[i];
    return NULL;
}
static uint64_t add(uint64_t now,uint64_t duration)
{return now>UINT64_MAX-duration?UINT64_MAX:now+duration;}
void lrq_init(lrq_t *q,const uint8_t boot[8])
{if(q && boot){memset(q,0,sizeof(*q));memcpy(q->boot,boot,8);}}
void lrq_close(lrq_t *q,const uint8_t route[16])
{if(q && route){lrq_slot_t *s=find(q,route);if(s)s->closing=true;}}
void lrq_close_all(lrq_t *q)
{if(q)for(unsigned i=0;i<LRQ_SLOTS;++i)if(q->slots[i].used)q->slots[i].closing=true;}
void lrq_expire(lrq_t *q,uint64_t now)
{
    if(!q)return;
    for(unsigned i=0;i<LRQ_SLOTS;++i) {
        lrq_slot_t *s=&q->slots[i];
        if(s->used && (now>=s->idle_deadline || ((s->queued||s->processing)&&now>=s->work.deadline_ms)))s->closing=true;
    }
}
bool lrq_take_closed(lrq_t *q,uint8_t route[16])
{
    if(!q||!route)return false;
    for(unsigned i=0;i<LRQ_SLOTS;++i)if(q->slots[i].used&&q->slots[i].closing&&!q->slots[i].processing) {
        memcpy(route,q->slots[i].work.route,16);memset(&q->slots[i],0,sizeof(q->slots[i]));return true;
    }
    return false;
}
static bool rate(lrq_t *q,lrq_role_t role,uint64_t now)
{
    unsigned total=0,guest=0;
    for(unsigned i=0;i<q->rate_count;++i)if(now>=q->rate_at[i] && now-q->rate_at[i]<1000) {
        ++total;if(q->rate_role[i]!=LRQ_OWNER)++guest;
    }
    if(total>=LRQ_RATE_TOTAL || (role!=LRQ_OWNER&&guest>=LRQ_RATE_GUEST))return false;
    q->rate_at[q->rate_cursor]=now;q->rate_role[q->rate_cursor]=role;
    q->rate_cursor=(q->rate_cursor+1)%LRQ_RATE_TOTAL;
    if(q->rate_count<LRQ_RATE_TOTAL)++q->rate_count;
    return true;
}
lrq_result_t lrq_submit(lrq_t *q,const lrq_request_t *r,uint64_t now)
{
    if(!q||!r||!r->id||r->operation>3||r->size>LRQ_PAYLOAD||(r->size&&!r->payload)||
        (r->operation==3?r->size!=0:r->size==0))return LRQ_ARGUMENT;
    if(memcmp(q->boot,r->boot,8))return LRQ_BOOT;
    uint32_t maximum=r->operation>=2?2000u:10000u;
    if(!r->ttl_ms||r->ttl_ms>maximum||r->lease_ms>now||r->lease_ms>UINT64_MAX-r->ttl_ms||
        now>=r->lease_ms+r->ttl_ms)return LRQ_EXPIRED;
    lrq_expire(q,now);
    lrq_slot_t *s=find(q,r->route),*empty=NULL;
    if(s&&s->closing)return LRQ_EXPIRED;
    if(s&&r->id<=s->last_id)return LRQ_REPLAY;
    if(s&&(s->queued||s->processing))return LRQ_IN_FLIGHT;
    unsigned pending_nonowner=0;
    for(unsigned i=0;i<LRQ_SLOTS;++i) {
        if(!q->slots[i].used&&!empty)empty=&q->slots[i];
        if((q->slots[i].queued||q->slots[i].processing)&&q->slots[i].role!=LRQ_OWNER)++pending_nonowner;
    }
    lrq_role_t role=s?s->role:LRQ_UNKNOWN;
    if((!s&&!empty)||(role!=LRQ_OWNER&&pending_nonowner>=LRQ_SLOTS-1))return LRQ_CAPACITY;
    if(!rate(q,role,now))return LRQ_RATE;
    if(!s){s=empty;memset(s,0,sizeof(*s));s->used=true;memcpy(s->work.route,r->route,16);
        s->idle_deadline=add(now,10000u);}
    s->last_id=s->work.id=r->id;s->work.operation=r->operation;
    s->work.deadline_ms=r->lease_ms+r->ttl_ms;s->work.size=(uint16_t)r->size;
    if(r->size)memcpy(s->work.payload,r->payload,r->size);
    /* An unproven route has one absolute admission window. Cheap identity
     * requests must not keep all slots occupied indefinitely. */
    if(role!=LRQ_UNKNOWN)s->idle_deadline=add(now,60000u);
    s->queued=true;
    return LRQ_OK;
}
bool lrq_take(lrq_t *q,lrq_work_t *out,uint64_t now)
{
    if(!q||!out)return false;
    lrq_expire(q,now);
    unsigned first=q->next_slot;
    for(unsigned i=0;i<LRQ_SLOTS;++i)if(q->slots[i].queued&&!q->slots[i].closing&&q->slots[i].role==LRQ_OWNER)first=i;
    for(unsigned n=0;n<LRQ_SLOTS;++n) {
        unsigned i=(first+n)%LRQ_SLOTS;lrq_slot_t *s=&q->slots[i];
        if(s->queued&&!s->closing){*out=s->work;s->queued=false;s->processing=true;q->next_slot=(i+1)%LRQ_SLOTS;return true;}
    }
    return false;
}
bool lrq_complete(lrq_t *q,const uint8_t route[16],uint32_t id,uint64_t now)
{
    if(!q||!route)return false;
    lrq_slot_t *s=find(q,route);
    if(!s||!s->processing||s->work.id!=id)return false;
    s->processing=false;
    bool live=!s->closing&&now<s->work.deadline_ms;
    if(!live)s->closing=true;
    memset(s->work.payload,0,sizeof(s->work.payload));s->work.size=0;
    return live;
}
bool lrq_set_role(lrq_t *q,const uint8_t route[16],lrq_role_t role,uint64_t now)
{
    if(!q||!route||role<LRQ_GUEST||role>LRQ_OWNER)return false;
    lrq_slot_t *s=find(q,route);
    if(!s||s->closing||now>=s->idle_deadline||(s->role!=LRQ_UNKNOWN&&s->role!=role))return false;
    s->role=role;s->idle_deadline=add(now,60000);return true;
}
bool lrq_admit_external(lrq_t *q,lrq_role_t role,uint64_t now)
{return q&&role>=LRQ_UNKNOWN&&role<=LRQ_OWNER&&rate(q,role,now);}
