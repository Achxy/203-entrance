#include "lighting_ble_frame.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t request(uint8_t *p, uint8_t op, uint8_t flags, uint32_t id,
                       uint16_t total, uint16_t offset, size_t size)
{
    p[0]=1; p[1]=op; p[2]=flags; p[3]=0;
    p[4]=(uint8_t)(id>>24); p[5]=(uint8_t)(id>>16); p[6]=(uint8_t)(id>>8); p[7]=(uint8_t)id;
    p[8]=(uint8_t)(total>>8); p[9]=(uint8_t)total;
    p[10]=(uint8_t)(offset>>8); p[11]=(uint8_t)offset;
    for(size_t i=0; i<size; ++i) p[12+i]=(uint8_t)(offset+i);
    return size+12;
}

int main(void)
{
    lb_receiver_t r; uint8_t p[512]; lb_receiver_init(&r);
    size_t n=request(p,LB_SESSION,LB_START,1,384,0,100);
    assert(lb_receiver_feed(&r,p,n,10)==LB_MORE);
    n=request(p,LB_SESSION,0,1,384,100,100);
    assert(lb_receiver_feed(&r,p,n,20)==LB_MORE);
    n=request(p,LB_SESSION,LB_END,1,384,200,184);
    assert(lb_receiver_feed(&r,p,n,30)==LB_COMPLETE);
    assert(r.used==384 && r.size==384 && r.id==1 && r.phase==LB_READY);
    for(size_t i=0;i<384;++i) assert(r.bytes[i]==(uint8_t)i);
    assert(lb_receiver_feed(&r,p,n,31)==LB_BUSY);
    lb_receiver_release(&r);
    n=request(p,LB_CLOSE,LB_START|LB_END,1,0,0,0);
    assert(lb_receiver_feed(&r,p,n,40)==LB_BAD_ORDER);
    p[7]=2;
    assert(lb_receiver_feed(&r,p,n,40)==LB_COMPLETE);
    lb_receiver_release(&r);
    n=request(p,LB_DATA,LB_START,3,1024,0,500);
    assert(lb_receiver_feed(&r,p,n,50)==LB_MORE);
    n=request(p,LB_DATA,0,3,1024,500,500);
    assert(lb_receiver_feed(&r,p,n,51)==LB_MORE);
    n=request(p,LB_DATA,LB_END,3,1024,1000,24);
    assert(lb_receiver_feed(&r,p,n,52)==LB_COMPLETE);
    assert(lb_receiver_expired(&r,10050));
    assert(!lb_receiver_expired(&r,10049));
    lb_receiver_release(&r);

    n=request(p,LB_SESSION,LB_START,4,100,0,20);
    assert(lb_receiver_feed(&r,p,n,100)==LB_MORE);
    assert(lb_receiver_feed(&r,p,n,101)==LB_BAD_ORDER);
    n=request(p,LB_SESSION,LB_END,4,100,19,81);
    assert(lb_receiver_feed(&r,p,n,102)==LB_BAD_ORDER);
    n=request(p,LB_SESSION,LB_END,4,100,21,79);
    assert(lb_receiver_feed(&r,p,n,103)==LB_BAD_ORDER);
    n=request(p,LB_SESSION,LB_END,4,100,20,80);
    assert(lb_receiver_feed(&r,p,n,10100)==LB_EXPIRED);
    lb_receiver_init(&r);
    n=request(p,LB_SESSION,LB_START|LB_END,1,1025,0,10);
    assert(lb_receiver_feed(&r,p,n,0)==LB_BAD_FRAME);
    n=request(p,LB_SESSION,LB_START,1,10,0,10);
    assert(lb_receiver_feed(&r,p,n,0)==LB_BAD_FRAME);
    n=request(p,LB_SESSION,LB_END,1,10,0,10);
    assert(lb_receiver_feed(&r,p,n,0)==LB_BAD_ORDER);
    n=request(p,LB_SESSION,LB_START|LB_END,0,10,0,10);
    assert(lb_receiver_feed(&r,p,n,0)==LB_BAD_FRAME);
    n=request(p,LB_SESSION,LB_START|LB_END,1,10,0,10);
    for(size_t i=0;i<n;++i) assert(lb_receiver_feed(&r,p,i,0)!=LB_COMPLETE);
    p[0]=2; assert(lb_receiver_feed(&r,p,n,0)==LB_BAD_FRAME);p[0]=1;
    p[3]=1; assert(lb_receiver_feed(&r,p,n,0)==LB_BAD_FRAME);p[3]=0;
    p[1]=0x82; assert(lb_receiver_feed(&r,p,n,0)==LB_BAD_FRAME);p[1]=LB_SESSION;
    p[2]=7; assert(lb_receiver_feed(&r,p,n,0)==LB_BAD_FRAME);

    uint8_t body[1026], joined[1026]; body[0]=0;body[1]=200;
    for(size_t i=2;i<sizeof(body);++i) body[i]=(uint8_t)i;
    for(size_t cap=20;cap<=512;cap+=7) {
        size_t at=0;
        while(at<sizeof(body)) {
            size_t next=0;
            size_t used=lb_response_fragment(p,cap,LB_DATA,17,body,sizeof(body),at,&next);
            assert(used>12 && used<=cap && next>at && next<=sizeof(body));
            assert(p[0]==1 && p[1]==0x82 && p[3]==0 && p[7]==17);
            assert((bool)(p[2]&LB_START)==(at==0));
            assert((bool)(p[2]&LB_END)==(next==sizeof(body)));
            memcpy(joined+at,p+12,used-12);at=next;
        }
        assert(!memcmp(joined,body,sizeof(body)));
    }
    size_t next;
    assert(!lb_response_fragment(p,12,LB_DATA,17,body,sizeof(body),0,&next));
    assert(!lb_response_fragment(p,128,LB_DATA,0,body,sizeof(body),0,&next));
    assert(!lb_response_fragment(p,128,LB_DATA,17,body,1027,0,&next));
    uint32_t random=0x203c3;
    for(unsigned round=0;round<100000;++round) {
        lb_receiver_init(&r);
        random=random*1664525u+1013904223u;
        size_t count=random%sizeof(p);
        for(size_t i=0;i<count;++i){random=random*1664525u+1013904223u;p[i]=(uint8_t)(random>>24);}
        (void)lb_receiver_feed(&r,p,count,round);
    }
    printf("PASS BLE framing, replay/order/timeout bounds, MTU fragmentation,100000 malformed packets; receiver=%zu B\n",sizeof(r));
}
