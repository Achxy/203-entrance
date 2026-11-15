#include "lighting_local_time_idf.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* HTTPS6 + arcade7 (each includes3 internal sockets) + relay1 + WiZ1 +
 * DNS/NTP2 + one outbound release/check reserve =18. Raw lwIP DNS/SNTP
 * client PCBs do not consume socket descriptors. Candidate uses20; default
 * firmware uses16 and must refuse these extra sockets. */
#define LLT_REQUIRED_SOCKETS 18
#define LLT_RETRY_MS 30000u

static void increment(uint32_t *count){if(*count!=UINT32_MAX)++*count;}
static uint64_t after(uint64_t now,uint64_t delay)
{return now>UINT64_MAX-delay?UINT64_MAX:now+delay;}
static void close_sockets(llt_idf_t *s)
{
    if(s->dns_fd>=0)close(s->dns_fd);
    if(s->ntp_fd>=0)close(s->ntp_fd);
    s->dns_fd=s->ntp_fd=-1;
}
void llt_idf_init(llt_idf_t *s,esp_netif_t *ap,
                  bool (*clock)(llt_clock_t *,void *),void *context)
{
    if(!s)return;
    memset(s,0,sizeof(*s));s->dns_fd=s->ntp_fd=-1;
    s->ap=ap;s->wall_clock=clock;s->context=context;s->initialized=true;s->dns_turn=true;
}
void llt_idf_stop(llt_idf_t *s)
{
    if(!s||!s->initialized)return;
    close_sockets(s);s->status.state=LLT_OFF;s->retry_at_ms=0;s->check_at_ms=0;
}
static llt_clock_t read_clock(llt_idf_t *s)
{
    llt_clock_t clock={0};
    if(!s->wall_clock||!s->wall_clock(&clock,s->context))clock.trusted=false;
    clock.trusted=llt_clock_valid(clock);
    return clock;
}
static llt_state_t configuration_state(llt_idf_t *s,esp_netif_ip_info_t *ip)
{
    if(CONFIG_LWIP_MAX_SOCKETS<LLT_REQUIRED_SOCKETS)return LLT_SOCKET_BUDGET_REFUSED;
    if(!s->ap||s->ap!=esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")||
       !(esp_netif_get_flags(s->ap)&ESP_NETIF_DHCP_SERVER)||!esp_netif_is_netif_up(s->ap))return LLT_WAIT_AP;
    wifi_mode_t mode;esp_netif_dhcp_status_t dhcp;
    if(esp_wifi_get_mode(&mode)!=ESP_OK||(mode!=WIFI_MODE_AP&&mode!=WIFI_MODE_APSTA)||
       esp_netif_dhcps_get_status(s->ap,&dhcp)!=ESP_OK||dhcp!=ESP_NETIF_DHCP_STARTED||
       esp_netif_get_ip_info(s->ap,ip)!=ESP_OK||!ip->ip.addr||!ip->netmask.addr)return LLT_WAIT_AP;
    /* Pinned IDF6.1 DHCP supplies APIP when both explicit servers are zero.
     * Read AP DHCP state, not global station DNS. Refuse conflicting overrides;
     * stopping DHCP to change them would clear current bulb/player leases. */
    for(esp_netif_dns_type_t type=ESP_NETIF_DNS_MAIN;type<=ESP_NETIF_DNS_BACKUP;++type) {
        esp_netif_dns_info_t dns={0};
        if(esp_netif_get_dns_info(s->ap,type,&dns)!=ESP_OK||dns.ip.type!=ESP_IPADDR_TYPE_V4||
           (dns.ip.u_addr.ip4.addr&&dns.ip.u_addr.ip4.addr!=ip->ip.addr))return LLT_DNS_CONFIGURATION_REFUSED;
    }
    return LLT_READY;
}
static int open_socket(llt_idf_t *s,uint16_t port)
{
    struct ifreq interface={0};
    if(esp_netif_get_netif_impl_name(s->ap,interface.ifr_name)!=ESP_OK)return -1;
    int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(fd<0)return -1;
    struct sockaddr_in local={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(s->ap_ip)};
    if(setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,&interface,sizeof(interface))!=0||
       bind(fd,(struct sockaddr *)&local,sizeof(local))!=0||fcntl(fd,F_SETFL,O_NONBLOCK)<0) {
        close(fd);return -1;
    }
    return fd;
}
static void failure(llt_idf_t *s,uint64_t now,llt_state_t state)
{
    close_sockets(s);s->status.state=state;s->retry_at_ms=after(now,LLT_RETRY_MS);
}
static bool current_peer(llt_idf_t *s,uint32_t source,uint8_t mac[6])
{
    esp_netif_ip_info_t ip;wifi_sta_list_t stations={0};
    if(!source||source==s->ap_ip||(source&s->netmask)!=(s->ap_ip&s->netmask)||
       !(source&~s->netmask)||(source&~s->netmask)==~s->netmask||
       (source>>24)>=224||!esp_netif_is_netif_up(s->ap)||
       esp_netif_get_ip_info(s->ap,&ip)!=ESP_OK||ntohl(ip.ip.addr)!=s->ap_ip||
       ntohl(ip.netmask.addr)!=s->netmask||esp_wifi_ap_get_sta_list(&stations)!=ESP_OK||
       stations.num<=0||stations.num>(int)LLT_PEERS)return false;
    esp_netif_pair_mac_ip_t pairs[LLT_PEERS]={0};
    for(int i=0;i<stations.num;++i)memcpy(pairs[i].mac,stations.sta[i].mac,6);
    if(esp_netif_dhcps_get_clients_by_mac(s->ap,stations.num,pairs)!=ESP_OK)return false;
    bool found=false;
    for(int i=0;i<stations.num;++i)if(ntohl(pairs[i].ip.addr)==source) {
        if(found)return false; /* Duplicate lease ownership is not an identity. */
        memcpy(mac,pairs[i].mac,6);found=true;
    }
    return found;
}
/* Returns true when a packet was consumed (including a rejected packet). */
static bool receive_one(llt_idf_t *s,bool dns,uint64_t now)
{
    uint8_t request[LLT_DNS_MAX+1],reply[LLT_DNS_MAX];
    struct sockaddr_in source={0};socklen_t source_size=sizeof(source);
    int fd=dns?s->dns_fd:s->ntp_fd;
    ssize_t count=recvfrom(fd,request,sizeof(request),MSG_DONTWAIT,(struct sockaddr *)&source,&source_size);
    if(count<0) {
        if(errno!=EAGAIN&&errno!=EWOULDBLOCK) {
            increment(&s->status.io_errors);failure(s,now,LLT_BIND_FAILED);return true;
        }
        return false;
    }
    llt_clock_t received={0};
    size_t size=(size_t)count,reply_size=0;
    if(source_size!=sizeof(source)||source.sin_family!=AF_INET||!source.sin_port||size>LLT_DNS_MAX||
       (dns?!(reply_size=llt_dns_reply(request,size,s->ap_ip,reply)):!llt_ntp_request(request,size))) {
        increment(&s->status.malformed);return true;
    }
    if(!dns)received=read_clock(s);
    uint32_t peer_ip=ntohl(source.sin_addr.s_addr);uint8_t peer_mac[6],fresh_mac[6];
    if(!current_peer(s,peer_ip,peer_mac)){increment(&s->status.foreign_peer);return true;}
    int64_t sample=esp_timer_get_time();
    if(sample<0||!llt_rate_allow(dns?&s->dns_rate:&s->ntp_rate,peer_mac,(uint64_t)sample/1000u)) {
        increment(&s->status.rate_limited);return true;
    }
    if(!current_peer(s,peer_ip,fresh_mac)||memcmp(fresh_mac,peer_mac,6)) {
        increment(&s->status.foreign_peer);return true;
    }
    if(!dns) {
        if(!llt_ntp_reply(request,size,received,read_clock(s),reply)) {
            increment(&s->status.clock_refused);s->status.state=LLT_WAIT_CLOCK;return true;
        }
        reply_size=LLT_NTP_BYTES;
    }
    ssize_t sent=sendto(fd,reply,reply_size,0,(struct sockaddr *)&source,sizeof(source));
    if(sent<0||(size_t)sent!=reply_size) {
        increment(&s->status.io_errors);failure(s,now,LLT_BIND_FAILED);return true;
    }
    increment(dns?&s->status.dns_replies:&s->status.ntp_replies);return true;
}
void llt_idf_poll(llt_idf_t *s,uint64_t now)
{
    if(!s||!s->initialized)return;
    if(s->dns_fd<0||s->ntp_fd<0) {
        if(now<s->retry_at_ms)return;
        esp_netif_ip_info_t ip;llt_state_t state=configuration_state(s,&ip);
        if(state!=LLT_READY){failure(s,now,state);return;}
        s->ap_ip=ntohl(ip.ip.addr);s->netmask=ntohl(ip.netmask.addr);
        s->dns_fd=open_socket(s,53);
        if(s->dns_fd>=0)s->ntp_fd=open_socket(s,123);
        if(s->dns_fd<0||s->ntp_fd<0) {
            increment(&s->status.io_errors);failure(s,now,LLT_BIND_FAILED);return;
        }
        s->check_at_ms=after(now,1000u);
    } else if(now>=s->check_at_ms) {
        esp_netif_ip_info_t ip;llt_state_t state=configuration_state(s,&ip);
        if(state!=LLT_READY){failure(s,now,state);return;}
        if(ntohl(ip.ip.addr)!=s->ap_ip||ntohl(ip.netmask.addr)!=s->netmask) {
            failure(s,now,LLT_WAIT_AP);return;
        }
        s->check_at_ms=after(now,1000u);
    }
    s->status.state=read_clock(s).trusted?LLT_READY:LLT_WAIT_CLOCK;
    bool dns=s->dns_turn;s->dns_turn=!dns;
    if(!receive_one(s,dns,now)) (void)receive_one(s,!dns,now);
}
