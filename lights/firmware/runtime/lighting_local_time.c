#include "lighting_local_time.h"
#include <string.h>

static void put32(uint8_t *p,uint32_t value)
{p[0]=(uint8_t)(value>>24);p[1]=(uint8_t)(value>>16);p[2]=(uint8_t)(value>>8);p[3]=(uint8_t)value;}
static uint16_t get16(const uint8_t *p){return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);}

const char *llt_state_name(llt_state_t state)
{
    switch(state) {
    case LLT_OFF:return "off";
    case LLT_WAIT_AP:return "waiting_ap";
    case LLT_SOCKET_BUDGET_REFUSED:return "socket_budget_refused";
    case LLT_DNS_CONFIGURATION_REFUSED:return "dns_configuration_refused";
    case LLT_BIND_FAILED:return "bind_failed";
    case LLT_WAIT_CLOCK:return "waiting_trusted_clock";
    case LLT_READY:return "ready";
    default:return "invalid";
    }
}

bool llt_rate_allow(llt_rate_t *r,const uint8_t mac[6],uint64_t now)
{
    if(!r||!mac||(mac[0]&1u))return false;
    bool nonzero=false;for(unsigned i=0;i<6;++i)nonzero|=mac[i]!=0;
    if(!nonzero||(r->initialized&&now<r->last_ms))return false;
    r->last_ms=now;r->initialized=true;
    unsigned kept=0;
    for(unsigned i=0;i<r->count;++i)
        if(now-r->recent[i]<LLT_RATE_MS)r->recent[kept++]=r->recent[i];
    r->count=(uint8_t)kept;
    if(kept==LLT_PEERS)return false;
    llt_peer_rate_t *slot=NULL;
    for(unsigned i=0;i<LLT_PEERS;++i) {
        llt_peer_rate_t *p=&r->peers[i];
        if(p->used&&!memcmp(p->mac,mac,6)) {
            if(now-p->last_ms<LLT_RATE_MS)return false;
            slot=p;break;
        }
        if(!slot&&(!p->used||now-p->last_ms>=LLT_RATE_MS))slot=p;
    }
    if(!slot)return false;
    memcpy(slot->mac,mac,6);slot->used=true;slot->last_ms=now;
    r->recent[r->count++]=now;return true;
}

bool llt_ntp_request(const uint8_t *p,size_t size)
{
    if(!p||size!=LLT_NTP_BYTES)return false;
    unsigned version=(p[0]>>3)&7u;
    return (version==3||version==4)&&(p[0]&7u)==3;
}
static bool timestamp(int64_t unix_seconds,uint32_t microseconds,uint8_t out[8])
{
    if(unix_seconds<0||unix_seconds>4102444800LL||microseconds>=1000000u)return false;
    uint32_t seconds=(uint32_t)((uint64_t)unix_seconds+2208988800ULL);
    uint32_t fraction=(uint32_t)(((uint64_t)microseconds<<32)/1000000u);
    if(!seconds&&!fraction)return false;
    put32(out,seconds);put32(out+4,fraction);return true;
}
bool llt_clock_valid(llt_clock_t t)
{
    uint8_t unused[8];
    if(!t.trusted||t.age_us>86400000000ULL||
       !timestamp(t.seconds,t.microseconds,unused)||
       !timestamp(t.reference_seconds,t.reference_microseconds,unused)||
       t.reference_seconds>t.seconds)return false;
    /* Validated Unix ranges make this subtraction/product signed64-safe. */
    int64_t elapsed=(t.seconds-t.reference_seconds)*1000000LL+
                    (int64_t)t.microseconds-(int64_t)t.reference_microseconds;
    return elapsed>=0&&(uint64_t)elapsed==t.age_us;
}
bool llt_ntp_reply(const uint8_t *request,size_t size,llt_clock_t received,
                   llt_clock_t transmitted,uint8_t reply[LLT_NTP_BYTES])
{
    uint8_t rx[8],tx[8],reference[8];
    if(!reply||!llt_ntp_request(request,size)||!llt_clock_valid(received)||!llt_clock_valid(transmitted)||
       transmitted.seconds<received.seconds||transmitted.seconds-received.seconds>2||
       (transmitted.seconds-received.seconds==2&&transmitted.microseconds>received.microseconds)||
       (transmitted.seconds==received.seconds&&transmitted.microseconds<received.microseconds))return false;
    (void)timestamp(received.seconds,received.microseconds,rx);
    (void)timestamp(transmitted.seconds,transmitted.microseconds,tx);
    (void)timestamp(transmitted.reference_seconds,transmitted.reference_microseconds,reference);
    uint8_t origin[8];memcpy(origin,request+40,sizeof(origin));
    uint8_t version=(uint8_t)(request[0]&0x38u);
    memset(reply,0,LLT_NTP_BYTES);reply[0]=(uint8_t)(version|4u);
    reply[1]=15;reply[2]=6;reply[3]=0; /* Conservative rank,64s poll,1s precision. */
    /* One-second floor +1000ppm age allowance, rounded upward to16.16.
     * Coarse engineering model: no measured upstream precision/stratum claim. */
    uint64_t uncertainty_us=1000000u+(transmitted.age_us+999u)/1000u;
    put32(reply+8,(uint32_t)((uncertainty_us*65536u+999999u)/1000000u));
    memcpy(reply+16,reference,8);memcpy(reply+24,origin,8);
    memcpy(reply+32,rx,8);memcpy(reply+40,tx,8);return true;
}

size_t llt_dns_reply(const uint8_t *p,size_t n,uint32_t ap,uint8_t out[LLT_DNS_MAX])
{
    if(!p||!out||n<17||n>LLT_DNS_MAX||!ap||
       (get16(p+2)&(uint16_t)~0x0100u)||get16(p+4)!=1||get16(p+6)||get16(p+8)||get16(p+10))return 0;
    static const uint8_t name[]={3,'n','t','p',3,'w','i','z',5,'w','o','r','l','d',0};
    size_t at=12;bool exact=true;
    for(;;) {
        if(at>=n)return 0;
        unsigned length=p[at];
        if(length>63)return 0; /* Includes every compression pointer. */
        if(at-12>=sizeof(name)||p[at]!=name[at-12])exact=false;
        ++at;if(!length)break;
        if(length>n-at)return 0;
        for(unsigned i=0;i<length;++i,++at) {
            uint8_t c=p[at];if(c>='A'&&c<='Z')c=(uint8_t)(c+('a'-'A'));
            if(at-12>=sizeof(name)||c!=name[at-12])exact=false;
        }
    }
    if(at+4!=n||get16(p+at+2)!=1)return 0;
    uint16_t type=get16(p+at);bool answer=exact&&type==1;
    if(answer&&n+16>LLT_DNS_MAX)return 0;
    memmove(out,p,n);out[2]=(uint8_t)(0x80u|(p[2]&1u)|(exact?4u:0u));
    out[3]=(uint8_t)(exact?0:3);out[6]=0;out[7]=(uint8_t)(answer?1:0);
    if(!answer)return n;
    uint8_t *rr=out+n;rr[0]=0xc0;rr[1]=0x0c;rr[2]=0;rr[3]=1;rr[4]=0;rr[5]=1;
    put32(rr+6,30);rr[10]=0;rr[11]=4;put32(rr+12,ap);return n+16;
}
