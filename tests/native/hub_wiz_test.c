#include "wiz_diag.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned sends,metas,bodies,done;
static uint32_t last_ip;
static char last_request[80],last_done[256];
static uint8_t allow[8][6];
static bool send_packet(void *ctx,uint32_t ip,const char *p,size_t n)
{
    (void)ctx;assert(n<sizeof(last_request));memcpy(last_request,p,n);last_request[n]=0;
    assert(!strstr(last_request,"set") && !strstr(last_request,"reset") && !strstr(last_request,"registration"));
    bool known=false;for(unsigned m=0;m<HD_METHOD_COUNT;++m){size_t size;const char *q=hd_request(m,&size);if(size==n && !memcmp(p,q,n))known=true;}
    assert(known);last_ip=ip;++sends;return true;
}
static bool emit_packet(void *ctx,uint8_t kind,uint32_t id,const void *p,size_t n)
{
    (void)ctx;assert(id==42 && n<=2048);
    if(kind==HD_META){assert(n==16);const uint8_t *b=p;assert(b[0]==1 && b[1]<5 && b[2]<8 && !b[3]);assert(!memcmp(b+8,allow[b[2]],6));++metas;}
    else if(kind==HD_BODY){assert(metas==bodies+1);++bodies;}
    else {assert(kind==HD_DONE && n<sizeof(last_done));memcpy(last_done,p,n);last_done[n]=0;++done;}
    return true;
}
static hd_state_t start(unsigned count)
{
    hd_state_t s={0};hd_io_t io={NULL,send_packet,emit_packet};
    sends=metas=bodies=done=0;
    for(unsigned i=0;i<8;++i){memset(allow[i],0,6);allow[i][0]=0x44;allow[i][5]=(uint8_t)(i+1);}
    assert(hd_start(&s,&io,42,0xc0a82b02,0xffffff00,&allow[0][0],count,100));
    return s;
}
static void response(hd_state_t *s,uint32_t ip,const char *json,uint64_t now)
{hd_receive(s,ip,38899,(const uint8_t *)json,strlen(json),now);}
int main(void)
{
    assert(hd_network(0xc0a82b02,0xffffff00));
    assert(!hd_network(0x08080808,0xffffff00));assert(!hd_network(0xc0a82b02,0xfffffffd));
    uint8_t raw[6];assert(hd_mac("44:00:00:00:00:01",raw));assert(!hd_mac("ffffffffffff",raw));
    size_t ignored;assert(!hd_request(5,&ignored));
    hd_state_t s=start(1);hd_poll(&s,100);assert(sends==1 && last_ip==0xc0a82bff);
    const char *system="{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"440000000001\",\"moduleName\":\"ESP24_SHRGB_01\"}}";
    response(&s,0x08080808,system,101);response(&s,0xc0a82b00,system,102);
    response(&s,0xc0a82b02,system,103);response(&s,0xc0a82bff,system,104);
    response(&s,0xc0a82b03,"{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"440000000002\"}}",105);
    response(&s,0xc0a82b03,"{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"440000000001\",\"mac\":\"440000000002\"}}",106);
    assert(!bodies);
    response(&s,0xc0a82b03,system,107);assert(bodies==1);
    response(&s,0xc0a82b03,system,108);assert(bodies==1);
    hd_poll(&s,700);assert(sends==2);hd_poll(&s,1699);assert(sends==2);
    hd_poll(&s,1700);assert(sends==3 && last_ip==0xc0a82b03 && strstr(last_request,"getModelConfig"));
    response(&s,0xc0a82b04,"{\"method\":\"getModelConfig\",\"result\":{}}",1701);assert(bodies==1);
    response(&s,0xc0a82b03,"{\"method\":\"getModelConfig\",\"result\":{\"mac\":\"440000000002\"}}",1702);assert(bodies==1);
    response(&s,0xc0a82b03,"{\"method\":\"setPilot\",\"result\":{\"success\":true}}",1703);assert(bodies==1);
    response(&s,0xc0a82b03,"{\"method\":\"getModelConfig\",\"result\":{\"whiteRange\":[2200,6500]}}",1704);
    hd_poll(&s,1710);assert(strstr(last_request,"getUserConfig"));
    response(&s,0xc0a82b03,"{\"method\":\"getUserConfig\",\"error\":{\"code\":-99}}",1711);
    hd_poll(&s,1720);assert(strstr(last_request,"getPilot"));
    response(&s,0xc0a82b03,"{\"method\":\"getPilot\",\"result\":{\"mac\":\"440000000002\",\"state\":true}}",1721);assert(bodies==3);
    response(&s,0xc0a82b03,"{\"method\":\"getPilot\",\"result\":{\"mac\":\"440000000001\",\"state\":true}}",1722);
    hd_poll(&s,1730);assert(strstr(last_request,"getTime"));
    response(&s,0xc0a82b03,"{\"method\":\"getTime\",\"result\":{\"utc\":1790441723}}",1731);
    hd_poll(&s,1740);assert(!s.active && done==1 && bodies==5 && strstr(last_done,"\"ok\":true"));

    s=start(1);hd_poll(&s,100);
    char maximum[2049];memset(maximum,' ',sizeof(maximum));memcpy(maximum,system,strlen(system));
    hd_receive(&s,0xc0a82b03,38899,(uint8_t *)maximum,2049,101);assert(!bodies && s.oversized==1);
    hd_receive(&s,0xc0a82b03,38899,(uint8_t *)maximum,2048,102);assert(bodies==1);
    response(&s,0xc0a82b04,system,103);assert(s.conflicts==1);
    hd_poll(&s,1700);assert(!s.active && sends==1); /* conflicted identity not targeted */
    s=start(1);hd_poll(&s,100);hd_poll(&s,99);assert(!s.active && strstr(last_done,"clock_rollback"));
    s=start(1);hd_poll(&s,25100);assert(!s.active && !sends);

    s=start(8);hd_poll(&s,100);
    for(unsigned i=0;i<8;++i){char json[160];snprintf(json,sizeof(json),"{\"method\":\"getSystemConfig\",\"result\":{\"mac\":\"4400000000%02x\"}}",i+1);response(&s,0xc0a82b03+i,json,101+i);}
    for(uint64_t now=120;now<25100 && s.active;now+=10)hd_poll(&s,now);
    assert(!s.active && sends==66 && s.timeouts==32 && bodies==8);
    s=start(1);hd_poll(&s,100);response(&s,0xc0a82b03,system,101);hd_poll(&s,1700);
    response(&s,0xc0a82b03,"{\"method\":\"getModelConfig\",\"error\":{\"code\":-99}}",1701);
    hd_poll(&s,1710);response(&s,0xc0a82b03,"{\"method\":\"getUserConfig\",\"error\":{\"code\":-99}}",1711);
    hd_poll(&s,1720);response(&s,0xc0a82b03,"{\"method\":\"getPilot\",\"error\":{\"code\":-99}}",1721);
    assert(bodies==4); /* Read rejection is recorded, never converted into a write. */
    /* Arbitrary datagrams exercise the bounded parser under sanitizers. */
    s=start(1);hd_poll(&s,100);uint32_t seed=19;uint8_t noise[2049];
    for(unsigned trial=0;trial<2000;++trial){for(unsigned i=0;i<sizeof(noise);++i){seed=seed*1664525u+1013904223u;noise[i]=(uint8_t)(seed>>24);}hd_receive(&s,0xc0a82b03,38899,noise,trial%2049,101);}
    assert(!bodies);
    puts("PASS bounded read-only WiZ discovery, filtering, replies, deadlines, oversize and parser corpus");
    return 0;
}
