#include "lighting_runtime_queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static lrq_request_t request(unsigned route,unsigned id,uint8_t op,uint64_t lease)
{
    static const uint8_t payload[]={1,2,3};
    lrq_request_t r={.id=id,.operation=op,.lease_ms=lease,.ttl_ms=op>=2?2000:10000,
                      .payload=op==3?NULL:payload,.size=op==3?0:sizeof(payload)};
    r.route[0]=(uint8_t)route;r.boot[0]=1;return r;
}
int main(void)
{
    lrq_t q;uint8_t boot[8]={1},closed[16];lrq_work_t work;
    lrq_init(&q,boot);
    lrq_request_t a=request(1,1,1,100),b=request(2,1,1,100),c=request(3,1,1,100);
    assert(lrq_submit(&q,&a,100)==LRQ_OK);
    assert(lrq_submit(&q,&a,100)==LRQ_REPLAY);
    a.id=2;assert(lrq_submit(&q,&a,100)==LRQ_IN_FLIGHT);a.id=1;
    assert(lrq_submit(&q,&b,100)==LRQ_OK);
    assert(lrq_submit(&q,&c,100)==LRQ_CAPACITY);
    assert(lrq_take(&q,&work,100)&&work.route[0]==1);
    assert(lrq_complete(&q,a.route,1,101));
    assert(lrq_set_role(&q,a.route,LRQ_OWNER,101));
    a.id=2;assert(lrq_submit(&q,&a,101)==LRQ_OK);
    assert(lrq_take(&q,&work,101)&&work.route[0]==1);
    assert(lrq_complete(&q,a.route,2,102));
    assert(lrq_submit(&q,&c,102)==LRQ_OK);
    a.id=3;assert(lrq_submit(&q,&a,102)==LRQ_OK); /* Owner-reserved pending capacity. */
    assert(lrq_take(&q,&work,102)&&work.route[0]==1);
    lrq_close(&q,a.route);assert(!lrq_take_closed(&q,closed));
    assert(!lrq_complete(&q,a.route,3,102));
    assert(lrq_take_closed(&q,closed)&&closed[0]==1);
    assert(lrq_take(&q,&work,102)&&work.route[0]==2);
    assert(lrq_complete(&q,b.route,1,103));
    lrq_close_all(&q);assert(lrq_take_closed(&q,closed));assert(lrq_take_closed(&q,closed));
    assert(!lrq_take_closed(&q,closed));

    lrq_init(&q,boot);a=request(1,1,2,100);
    assert(lrq_submit(&q,&a,2100)==LRQ_EXPIRED);
    assert(lrq_submit(&q,&a,99)==LRQ_EXPIRED);
    a.boot[0]=2;assert(lrq_submit(&q,&a,100)==LRQ_BOOT);a.boot[0]=1;
    a.ttl_ms=2001;assert(lrq_submit(&q,&a,100)==LRQ_EXPIRED);a.ttl_ms=2000;
    assert(lrq_submit(&q,&a,100)==LRQ_OK);
    assert(!lrq_take(&q,&work,2100));assert(lrq_take_closed(&q,closed));
    a=request(1,1,1,UINT64_MAX-1);assert(lrq_submit(&q,&a,UINT64_MAX-1)==LRQ_EXPIRED);

    lrq_init(&q,boot);
    for(unsigned i=1;i<=6;++i) {
        a=request(1,i,1,100);assert(lrq_submit(&q,&a,100)==LRQ_OK);
        assert(lrq_take(&q,&work,100));assert(lrq_complete(&q,a.route,i,100));
    }
    a.id=7;assert(lrq_submit(&q,&a,100)==LRQ_RATE);
    assert(lrq_set_role(&q,a.route,LRQ_OWNER,100));
    assert(lrq_submit(&q,&a,100)==LRQ_OK);assert(lrq_take(&q,&work,100));assert(lrq_complete(&q,a.route,7,100));
    a.id=8;assert(lrq_submit(&q,&a,100)==LRQ_OK);assert(lrq_take(&q,&work,100));assert(lrq_complete(&q,a.route,8,100));
    a.id=9;assert(lrq_submit(&q,&a,100)==LRQ_RATE);
    assert(lrq_submit(&q,&a,1100)==LRQ_OK);assert(lrq_take(&q,&work,1100));assert(lrq_complete(&q,a.route,9,1100));
    assert(!lrq_set_role(&q,a.route,LRQ_GUEST,1100));
    lrq_expire(&q,61100);assert(lrq_take_closed(&q,closed));
    lrq_init(&q,boot);a=request(1,1,3,100);a.ttl_ms=10000;
    assert(lrq_submit(&q,&a,100)==LRQ_EXPIRED);a.ttl_ms=2000;
    assert(lrq_submit(&q,&a,100)==LRQ_OK);assert(lrq_take(&q,&work,100));
    assert(lrq_complete(&q,a.route,1,101)); /* Close ACK can be delivered first. */
    lrq_close(&q,a.route);assert(lrq_take_closed(&q,closed));
    lrq_init(&q,boot);
    for(unsigned i=0;i<LRQ_RATE_GUEST;++i)assert(lrq_admit_external(&q,LRQ_GUEST,100));
    a=request(1,1,2,100);assert(lrq_submit(&q,&a,100)==LRQ_RATE);
    assert(lrq_admit_external(&q,LRQ_OWNER,100));assert(lrq_admit_external(&q,LRQ_OWNER,100));
    assert(!lrq_admit_external(&q,LRQ_OWNER,100));
    assert(lrq_submit(&q,&a,1100)==LRQ_OK);
    lrq_init(&q,boot);a=request(1,1,0,100);
    assert(lrq_submit(&q,&a,100)==LRQ_OK);
    assert(lrq_take(&q,&work,100));assert(lrq_complete(&q,a.route,1,101));
    a=request(1,2,0,9100);assert(lrq_submit(&q,&a,9100)==LRQ_OK);
    assert(lrq_take(&q,&work,9100));assert(lrq_complete(&q,a.route,2,9101));
    assert(!lrq_set_role(&q,a.route,LRQ_OWNER,10100));
    a=request(1,3,0,10100);assert(lrq_submit(&q,&a,10100)==LRQ_EXPIRED);
    assert(lrq_take_closed(&q,closed)&&closed[0]==1);
    /* A real proof before the absolute deadline earns the authenticated idle window. */
    lrq_init(&q,boot);a=request(1,1,1,100);
    assert(lrq_submit(&q,&a,100)==LRQ_OK);assert(lrq_take(&q,&work,100));
    assert(lrq_complete(&q,a.route,1,101));assert(lrq_set_role(&q,a.route,LRQ_OWNER,10099));
    lrq_expire(&q,10100);assert(!lrq_take_closed(&q,closed));
    lrq_expire(&q,70099);assert(lrq_take_closed(&q,closed));
    printf("PASS runtime queue deadlines/replay/roles/owner capacity/close races/sliding rate/absolute preauth expiry; queue=%zu bytes\n",sizeof(q));
}
