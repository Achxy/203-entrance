#include "wiz_diag.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
static const char *const methods[] = {
    "getSystemConfig", "getModelConfig", "getUserConfig", "getPilot", "getTime"
};
static const char *const requests[] = {
    "{\"method\":\"getSystemConfig\",\"params\":{}}",
    "{\"method\":\"getModelConfig\",\"params\":{}}",
    "{\"method\":\"getUserConfig\",\"params\":{}}",
    "{\"method\":\"getPilot\",\"params\":{}}",
    "{\"method\":\"getTime\",\"params\":{}}"
};
const char *hd_request(unsigned method, size_t *size)
{
    if (!size || method >= HD_METHOD_COUNT) return NULL;
    *size = strlen(requests[method]); return requests[method];
}
static bool private_ip(uint32_t ip)
{ return (ip >> 24) == 10 || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8; }
bool hd_network(uint32_t ip, uint32_t mask)
{
    uint32_t inverse = ~mask, host = ip & inverse;
    return private_ip(ip) && inverse >= 3 && inverse <= 0x00ffffff &&
        !(inverse & (inverse + 1)) && host && host != inverse;
}
bool hd_peer(const hd_state_t *s, uint32_t ip)
{
    return s && private_ip(ip) && ip != s->local_ip &&
        (ip & s->netmask) == (s->local_ip & s->netmask) &&
        (ip & ~s->netmask) && ip != s->broadcast;
}
static int nibble(unsigned char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool raw_mac(const uint8_t mac[6])
{
    unsigned any=0;for(unsigned i=0;i<6;++i)any|=mac[i];
    return any && !(mac[0] & 1);
}
bool hd_mac(const char *text, uint8_t out[6])
{
    if(!text || !out)return false;
    size_t n=strlen(text);if(n!=12 && n!=17)return false;
    uint8_t value[6];
    for(unsigned i=0;i<6;++i){
        unsigned at=i*(n==17?3u:2u);
        int a=nibble((unsigned char)text[at]),b=nibble((unsigned char)text[at+1]);
        if(a<0 || b<0 || (n==17 && i<5 && text[at+2]!=':'))return false;
        value[i]=(uint8_t)((a<<4)|b);
    }
    if(!raw_mac(value))return false;
    memcpy(out,value,6);return true;
}
/* Bounds cJSON allocation; no response JSON ever becomes an outgoing request. */
static bool budget(const uint8_t *p,size_t n)
{
    bool quoted=false,escape=false;unsigned depth=0,tokens=0;
    for(size_t i=0;i<n;++i){
        unsigned c=p[i];if(!c)return false;
        if(quoted){
            if(escape){
                if(c=='u' && i+4<n && !memcmp(p+i+1,"0000",4))return false;
                escape=false;
            }else if(c=='\\')escape=true;
            else if(c=='"')quoted=false;
            else if(c<32)return false;
        }else if(c=='"'){quoted=true;++tokens;}
        else if(c=='{' || c=='['){if(++depth>6)return false;++tokens;}
        else if(c=='}' || c==']'){if(!depth)return false;--depth;}
        else if(c==',' || c==':')++tokens;
        if(tokens>192)return false;
    }
    return !quoted && !escape && !depth;
}
static bool unique(const cJSON *j)
{
    if(cJSON_IsObject(j))for(const cJSON *a=j->child;a;a=a->next)
        for(const cJSON *b=a->next;b;b=b->next)
            if(!strcmp(a->string,b->string))return false;
    for(const cJSON *c=j->child;c;c=c->next)if(!unique(c))return false;
    return true;
}
static bool envelope(const uint8_t *data,size_t n,unsigned expected,
                     bool require_mac,const uint8_t *wanted,uint8_t found[6])
{
    if(!n || n>HD_PACKET_MAX || !budget(data,n))return false;
    const char *end=NULL;
    cJSON *root=cJSON_ParseWithLengthOpts((const char *)data,n,&end,false);
    if(!root)return false;
    bool valid=false;
    while(end<(const char *)data+n && (*end==' ' || *end=='\t' || *end=='\r' || *end=='\n'))++end;
    if(end!=(const char *)data+n || !cJSON_IsObject(root) || !unique(root))goto done;
    const cJSON *m=cJSON_GetObjectItemCaseSensitive(root,"method");
    const cJSON *r=cJSON_GetObjectItemCaseSensitive(root,"result");
    const cJSON *e=cJSON_GetObjectItemCaseSensitive(root,"error");
    if(!cJSON_IsString(m) || strcmp(m->valuestring,methods[expected]) ||
       ((r!=NULL)==(e!=NULL)))goto done;
    if(e){valid=(!require_mac || wanted!=NULL) && cJSON_IsObject(e);goto done;}
    if(!cJSON_IsObject(r))goto done;
    const cJSON *mac=cJSON_GetObjectItemCaseSensitive(r,"mac");
    if(mac){
        if(!cJSON_IsString(mac) || !hd_mac(mac->valuestring,found) ||
           (wanted && memcmp(wanted,found,6)))goto done;
    }else if(require_mac)goto done;
    valid=true;
done:
    cJSON_Delete(root);return valid;
}
bool hd_start(hd_state_t *s,const hd_io_t *io,uint32_t request,uint32_t ip,
              uint32_t mask,const uint8_t *allow,size_t count,uint64_t now)
{
    if(!s || !io || !io->send || !io->emit || !request || !allow ||
       !count || count>HD_MAX_TARGETS || !hd_network(ip,mask) ||
       now>UINT64_MAX-HD_WINDOW_MS || s->active)return false;
    for(size_t i=0;i<count;++i){
        if(!raw_mac(allow+6*i))return false;
        for(size_t j=0;j<i;++j)if(!memcmp(allow+6*i,allow+6*j,6))return false;
    }
    memset(s,0,sizeof(*s));s->io=*io;s->request=request;s->local_ip=ip;s->netmask=mask;
    s->broadcast=ip|~mask;s->count=(uint8_t)count;s->last_now=now;
    s->deadline=now+HD_WINDOW_MS;s->discover_until=now+1600;s->next_send=now;
    s->active=s->discovery=true;
    for(size_t i=0;i<count;++i)memcpy(s->targets[i].mac,allow+6*i,6);
    return true;
}
void hd_stop(hd_state_t *s,const char *reason)
{
    if(!s || !s->active)return;
    s->active=false;unsigned found=0;
    for(unsigned i=0;i<s->count;++i)if(s->targets[i].ip && !s->targets[i].conflict)++found;
    char json[240];int n=snprintf(json,sizeof(json),
        "{\"ok\":%s,\"reason\":\"%s\",\"found\":%u,\"replies\":%lu,\"ignored\":%lu,"
        "\"oversized\":%lu,\"conflicts\":%lu,\"timeouts\":%lu}",
        !strcmp(reason,"complete")?"true":"false",reason,found,(unsigned long)s->replies,
        (unsigned long)s->ignored,(unsigned long)s->oversized,
        (unsigned long)s->conflicts,(unsigned long)s->timeouts);
    if(n>0 && n<(int)sizeof(json))s->io.emit(s->io.context,HD_DONE,s->request,json,(size_t)n);
}
static bool fresh(hd_state_t *s,uint64_t now)
{
    if(!s || !s->active)return false;
    if(now<s->last_now){hd_stop(s,"clock_rollback");return false;}
    s->last_now=now;
    if(now>=s->deadline){hd_stop(s,"time_budget");return false;}
    return true;
}
static void transmit(hd_state_t *s,uint32_t ip,uint64_t now)
{
    size_t n;const char *request=hd_request(s->method,&n);
    if(!request || !s->io.send(s->io.context,ip,request,n)){hd_stop(s,"send_failed");return;}
    ++s->sends;s->next_send=now+600;
}
void hd_poll(hd_state_t *s,uint64_t now)
{
    if(!fresh(s,now))return;
    if(s->discovery){
        if(now<s->discover_until){
            if(s->sends<2 && now>=s->next_send)transmit(s,s->broadcast,now);
            return;
        }
        s->discovery=false;s->method=HD_MODEL;s->sends=0;s->next_send=now;
    }
    for(unsigned steps=0;steps<HD_MAX_TARGETS*HD_METHOD_COUNT;++steps){
        if(s->index>=s->count){hd_stop(s,"complete");return;}
        hd_target_t *t=&s->targets[s->index];
        if(!t->ip || t->conflict){++s->index;s->method=HD_MODEL;s->sends=0;s->answered=false;continue;}
        if(s->answered || (s->sends && now>=s->query_until)){
            if(!s->answered)++s->timeouts;
            s->answered=false;s->sends=0;s->next_send=now;
            if(++s->method==HD_METHOD_COUNT){++s->index;s->method=HD_MODEL;}
            continue;
        }
        if(!s->sends)s->query_until=now+700;
        if(s->sends<2 && now>=s->next_send)transmit(s,t->ip,now);
        return;
    }
    hd_stop(s,"internal_bound");
}
void hd_receive(hd_state_t *s,uint32_t ip,uint16_t port,const uint8_t *data,size_t n,uint64_t now)
{
    if(!fresh(s,now))return;
    if(port!=38899 || !hd_peer(s,ip)){++s->ignored;return;}
    if(n>HD_PACKET_MAX){++s->oversized;return;}
    if(!data || !n || (!s->discovery && (!s->sends || s->answered || now>=s->query_until)) ||
       (s->discovery && (!s->sends || now>=s->discover_until))){++s->ignored;return;}
    uint8_t mac[6]={0};
    unsigned index=s->index;
    if(s->discovery){
        if(!envelope(data,n,HD_SYSTEM,true,NULL,mac)){++s->ignored;return;}
        for(index=0;index<s->count;++index)if(!memcmp(mac,s->targets[index].mac,6))break;
        if(index==s->count){++s->ignored;return;}
        hd_target_t *t=&s->targets[index];
        if(t->conflict){++s->ignored;return;}
        if(t->ip){
            if(t->ip!=ip){t->conflict=true;++s->conflicts;}
            else ++s->ignored;
            return;
        }
        for(unsigned i=0;i<s->count;++i)if(i!=index && s->targets[i].ip==ip){
            s->targets[i].conflict=t->conflict=true;++s->conflicts;return;
        }
        t->ip=ip;
    }else{
        if(index>=s->count || ip!=s->targets[index].ip ||
           !envelope(data,n,s->method,s->method==HD_PILOT,s->targets[index].mac,mac)){
            ++s->ignored;return;
        }
        s->answered=true;
    }
    uint8_t meta[16]={1,s->method,(uint8_t)index,0};
    for(unsigned i=0;i<4;++i)meta[4+i]=(uint8_t)(ip>>(24-8*i));
    memcpy(meta+8,s->targets[index].mac,6);meta[14]=(uint8_t)n;meta[15]=(uint8_t)(n>>8);
    if(!s->io.emit(s->io.context,HD_META,s->request,meta,sizeof(meta)) ||
       !s->io.emit(s->io.context,HD_BODY,s->request,data,n)){hd_stop(s,"usb_failed");return;}
    ++s->replies;
}
