#include "lighting_local_time.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t u32(const uint8_t *p)
{return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static size_t query(uint8_t *out,const uint8_t *name,size_t size,uint16_t type)
{
    memset(out,0,LLT_DNS_MAX);out[0]=0xab;out[1]=0xcd;out[2]=1;out[5]=1;
    memcpy(out+12,name,size);out[12+size]=(uint8_t)(type>>8);out[13+size]=(uint8_t)type;
    out[15+size]=1;return size+16;
}
static const uint8_t time_name[]={3,'n','t','p',3,'w','i','z',5,'w','o','r','l','d',0};
static llt_clock_t sample(int64_t seconds,uint32_t microseconds,uint64_t age)
{
    int64_t reference=seconds*1000000+(int64_t)microseconds-(int64_t)age;
    return (llt_clock_t){.seconds=seconds,.microseconds=microseconds,.trusted=true,
        .reference_seconds=reference/1000000,.reference_microseconds=(uint32_t)(reference%1000000),.age_us=age};
}
static void ntp(void)
{
    uint8_t p[LLT_NTP_BYTES]={0},out[LLT_NTP_BYTES],saved[8];
    for(unsigned i=0;i<8;++i)saved[i]=p[40+i]=(uint8_t)(i+10);
    llt_clock_t clock=sample(1794830400,500000,20000000);
    for(unsigned b=0;b<256;++b) {
        p[0]=(uint8_t)b;
        bool valid=((b>>3)&7u)==3||((b>>3)&7u)==4;valid=valid&&(b&7u)==3;
        assert(llt_ntp_request(p,sizeof(p))==valid);
        assert(llt_ntp_reply(p,sizeof(p),clock,clock,out)==valid);
        if(valid) {
            assert(out[0]==((b&0x38u)|4u));assert(out[1]==15&&out[3]==0);
            assert(u32(out+8)==66847);assert(!memcmp(out+24,saved,8));
            assert(u32(out+40)==(uint32_t)((uint64_t)clock.seconds+2208988800ULL));
            assert(u32(out+44)==0x80000000u);
            assert(u32(out+16)==u32(out+32)-20&&u32(out+20)==u32(out+36));
        }
    }
    p[0]=0xe3; /* An unsynchronized version4 client is exactly who needs time. */
    assert(llt_ntp_request(p,sizeof(p)));
    for(size_t size=0;size<LLT_DNS_MAX;++size)if(size!=48)assert(!llt_ntp_request(p,size));
    assert(!llt_ntp_request(NULL,48));assert(!llt_ntp_reply(p,48,clock,clock,NULL));
    llt_clock_t bad=clock;bad.trusted=false;assert(!llt_ntp_reply(p,48,bad,clock,out));
    assert(!llt_ntp_reply(p,48,clock,bad,out));
    bad=clock;bad.seconds=-1;assert(!llt_ntp_reply(p,48,bad,bad,out));
    bad.seconds=4102444801LL;assert(!llt_ntp_reply(p,48,bad,bad,out));
    bad=clock;bad.microseconds=1000000;assert(!llt_ntp_reply(p,48,bad,bad,out));
    bad=clock;--bad.seconds;assert(!llt_ntp_reply(p,48,clock,bad,out));
    bad=clock;--bad.microseconds;assert(!llt_ntp_reply(p,48,clock,bad,out));
    bad=clock;bad.seconds+=3;assert(!llt_ntp_reply(p,48,clock,bad,out));
    bad=clock;bad.seconds+=2;bad.age_us+=2000001;++bad.microseconds;assert(!llt_ntp_reply(p,48,clock,bad,out));
    --bad.microseconds;--bad.age_us;assert(llt_ntp_reply(p,48,clock,bad,out));
    bad=clock;++bad.age_us;assert(!llt_clock_valid(bad));
    bad=clock;bad.reference_seconds=bad.seconds+1;assert(!llt_clock_valid(bad));
    bad=sample(1794830400,0,86400000000ULL);assert(llt_clock_valid(bad));
    assert(llt_ntp_reply(p,48,bad,bad,out));assert(u32(out+8)==5727847u); /*87.4s, round up. */
    ++bad.age_us;assert(!llt_clock_valid(bad));bad.age_us=UINT64_MAX;assert(!llt_clock_valid(bad));
    clock=sample(0,0,0);
    assert(llt_ntp_reply(p,48,clock,clock,out));assert(u32(out+40)==2208988800u);
    clock=sample(2085978495,0,0);assert(llt_ntp_reply(p,48,clock,clock,out));assert(u32(out+40)==UINT32_MAX);
    clock=sample(2085978496,0,0);assert(!llt_ntp_reply(p,48,clock,clock,out)); /* Reserved zero timestamp at era wrap. */
    clock=sample(2085978496,1,0);assert(llt_ntp_reply(p,48,clock,clock,out));
    assert(u32(out+40)==0&&u32(out+44)==4294);
    clock=sample(2085978497,0,0);assert(llt_ntp_reply(p,48,clock,clock,out));assert(u32(out+40)==1);
    clock=sample(4102444800LL,0,0);assert(llt_ntp_reply(p,48,clock,clock,out));
    memset(p+40,0,8);assert(llt_ntp_reply(p,48,clock,clock,out));assert(!u32(out+24)&&!u32(out+28));
    memcpy(p+40,saved,8);assert(llt_ntp_reply(p,48,clock,clock,p));assert(!memcmp(p+24,saved,8));
}
static void dns(void)
{
    uint8_t p[LLT_DNS_MAX+1],out[LLT_DNS_MAX],copy[LLT_DNS_MAX];
    size_t n=query(p,time_name,sizeof(time_name),1);
    assert(n==31);assert(llt_dns_reply(p,n,0xc0a80401u,out)==47);
    assert(out[0]==0xab&&out[1]==0xcd&&out[2]==0x85&&out[3]==0&&out[7]==1);
    assert(!memcmp(out+12,p+12,n-12));assert(u32(out+n+6)==30&&u32(out+n+12)==0xc0a80401u);
    assert(out[n]==0xc0&&out[n+1]==12);assert(out[n+11]==4);
    memcpy(copy,p,n);assert(llt_dns_reply(copy,n,0xc0a80401u,copy)==47);assert(!memcmp(copy,out,47));
    p[13]='N';p[17]='W';assert(llt_dns_reply(p,n,0xc0a80401u,out)==47);
    p[n-3]=28;assert(llt_dns_reply(p,n,0xc0a80401u,out)==n);assert(!out[7]&&!out[3]);
    p[n-3]=255;assert(llt_dns_reply(p,n,0xc0a80401u,out)==n);assert(!out[7]&&!out[3]);
    p[13]='x';assert(llt_dns_reply(p,n,0xc0a80401u,out)==n);assert(out[3]==3&&!out[7]);
    uint8_t root[]={0};n=query(p,root,1,1);assert(llt_dns_reply(p,n,0xc0a80401u,out)==n);
    assert(out[3]==3);
    n=query(p,time_name,sizeof(time_name),1);
    for(size_t cut=0;cut<n;++cut)assert(!llt_dns_reply(p,cut,0xc0a80401u,out));
    p[n]=0;assert(!llt_dns_reply(p,n+1,0xc0a80401u,out));
    assert(!llt_dns_reply(p,97,0xc0a80401u,out));assert(!llt_dns_reply(p,n,0,out));
    assert(!llt_dns_reply(NULL,n,0xc0a80401u,out));assert(!llt_dns_reply(p,n,0xc0a80401u,NULL));
    for(unsigned flag=0;flag<16;++flag) {
        n=query(p,time_name,sizeof(time_name),1);p[2]=(uint8_t)((1u<<flag)>>8);p[3]=(uint8_t)(1u<<flag);
        assert((llt_dns_reply(p,n,0xc0a80401u,out)!=0)==(flag==8));
    }
    n=query(p,time_name,sizeof(time_name),1);p[5]=2;assert(!llt_dns_reply(p,n,1,out));
    n=query(p,time_name,sizeof(time_name),1);p[7]=1;assert(!llt_dns_reply(p,n,1,out));
    n=query(p,time_name,sizeof(time_name),1);p[9]=1;assert(!llt_dns_reply(p,n,1,out));
    n=query(p,time_name,sizeof(time_name),1);p[11]=1;assert(!llt_dns_reply(p,n,1,out)); /* EDNS refused. */
    n=query(p,time_name,sizeof(time_name),1);p[n-1]=3;assert(!llt_dns_reply(p,n,1,out));
    n=query(p,time_name,sizeof(time_name),1);p[12]=0xc0;p[13]=0x0c;assert(!llt_dns_reply(p,n,1,out));
    for(unsigned length=64;length<256;++length){p[12]=(uint8_t)length;assert(!llt_dns_reply(p,n,1,out));}
    uint32_t random=1;
    struct { uint8_t pre[8],bytes[LLT_DNS_MAX],post[8]; } guard;
    memset(&guard,0xa5,sizeof(guard));
    for(unsigned trial=0;trial<20000;++trial) {
        size_t size=trial%(LLT_DNS_MAX+2u);
        for(size_t i=0;i<size;++i){random=random*1664525u+1013904223u;p[i]=(uint8_t)(random>>24);}
        size_t reply=llt_dns_reply(p,size,0xc0a80401u,guard.bytes);
        assert(reply<=LLT_DNS_MAX);
        for(unsigned i=0;i<8;++i)assert(guard.pre[i]==0xa5&&guard.post[i]==0xa5);
    }
}
static void rates(void)
{
    llt_rate_t rate={0};uint8_t mac[6]={2,0,0,0,0,1};
    assert(llt_rate_allow(&rate,mac,0));
    for(unsigned i=0;i<100;++i)assert(!llt_rate_allow(&rate,mac,0));
    for(unsigned i=2;i<=8;++i){mac[5]=(uint8_t)i;assert(llt_rate_allow(&rate,mac,0));}
    mac[5]=9;assert(!llt_rate_allow(&rate,mac,0));assert(!llt_rate_allow(&rate,mac,999));
    assert(llt_rate_allow(&rate,mac,1000));assert(!llt_rate_allow(&rate,mac,999));
    for(unsigned i=1;i<=7;++i){mac[5]=(uint8_t)i;assert(llt_rate_allow(&rate,mac,1000));}
    mac[5]=8;assert(!llt_rate_allow(&rate,mac,1000));
    assert(llt_rate_allow(&rate,mac,2000));
    mac[0]=1;assert(!llt_rate_allow(&rate,mac,3000));
    memset(mac,0,6);assert(!llt_rate_allow(&rate,mac,3000));
    assert(!llt_rate_allow(NULL,mac,0));assert(!llt_rate_allow(&rate,NULL,0));
    memset(&rate,0,sizeof(rate));mac[0]=2;
    assert(llt_rate_allow(&rate,mac,UINT64_MAX-999));assert(!llt_rate_allow(&rate,mac,UINT64_MAX));
    assert(!llt_rate_allow(&rate,mac,0));
}
int main(void)
{
    ntp();dns();rates();
    assert(!strcmp(llt_state_name(LLT_SOCKET_BUDGET_REFUSED),"socket_budget_refused"));
    assert(!strcmp(llt_state_name(LLT_DNS_CONFIGURATION_REFUSED),"dns_configuration_refused"));
    puts("local time: bounded DNS/NTP, era rollover, hostile inputs and per-peer/global fairness passed");
    return 0;
}
