#include "lighting_local_time_idf.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint8_t bytes[97];size_t size;uint32_t ip;uint16_t port;bool queued; } packet_t;
static esp_netif_t ap={1},sta={2};
static struct {
    bool up,trusted,peer,dhcp,send_fail,recv_fail;
    unsigned opened,closed,sent,received,lease_checks,lose_lease_at,fail_open_at;
    uint32_t dns,ap_ip;
    uint64_t now;
    int last_fd;
    uint8_t reply[96];size_t reply_size;
    packet_t dns_packet,ntp_packet;
} mock;
static const uint8_t mac[6]={2,3,4,5,6,7};
static void reset(void)
{
    memset(&mock,0,sizeof(mock));mock.up=mock.trusted=mock.peer=mock.dhcp=true;
    mock.ap_ip=0xc0a80401u;
}
static bool clock_read(llt_clock_t *sample,void *context)
{
    assert(context==&mock);
    *sample=(llt_clock_t){.seconds=1794830400+(int64_t)(mock.now/1000),
        .microseconds=(uint32_t)(mock.now%1000)*1000u,.trusted=mock.trusted,
        .reference_seconds=1794830400,.age_us=mock.now*1000};
    return mock.trusted;
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{assert(!strcmp(key,"WIFI_AP_DEF"));return &ap;}
unsigned esp_netif_get_flags(esp_netif_t *net){return net==&ap?ESP_NETIF_DHCP_SERVER:0;}
bool esp_netif_is_netif_up(esp_netif_t *net){return net==&ap&&mock.up;}
int esp_netif_get_ip_info(esp_netif_t *net,esp_netif_ip_info_t *ip)
{assert(net==&ap);*ip=(esp_netif_ip_info_t){.ip.addr=htonl(mock.ap_ip),.netmask.addr=htonl(0xffffff00u)};return 0;}
int esp_netif_dhcps_get_status(esp_netif_t *net,esp_netif_dhcp_status_t *state)
{assert(net==&ap);*state=mock.dhcp?ESP_NETIF_DHCP_STARTED:ESP_NETIF_DHCP_STOPPED;return 0;}
int esp_netif_get_dns_info(esp_netif_t *net,esp_netif_dns_type_t type,esp_netif_dns_info_t *dns)
{assert(net==&ap);(void)type;memset(dns,0,sizeof(*dns));dns->ip.u_addr.ip4.addr=htonl(mock.dns);return 0;}
int esp_netif_get_netif_impl_name(esp_netif_t *net,char *name)
{assert(net==&ap);strcpy(name,"ap");return 0;}
int esp_netif_dhcps_get_clients_by_mac(esp_netif_t *net,int count,esp_netif_pair_mac_ip_t *pairs)
{
    assert(net==&ap&&count==1&&!memcmp(pairs[0].mac,mac,6));++mock.lease_checks;
    bool lost=mock.lose_lease_at&&mock.lease_checks>=mock.lose_lease_at;
    pairs[0].ip.addr=lost?0:htonl(0xc0a80402u);return 0;
}
int esp_wifi_get_mode(wifi_mode_t *mode){*mode=WIFI_MODE_APSTA;return 0;}
int esp_wifi_ap_get_sta_list(wifi_sta_list_t *stations)
{memset(stations,0,sizeof(*stations));stations->num=mock.peer?1:0;memcpy(stations->sta[0].mac,mac,6);return 0;}
int64_t esp_timer_get_time(void){return (int64_t)mock.now*1000;}
int llt_test_socket(int domain,int type,int protocol)
{
    assert(domain==AF_INET&&type==SOCK_DGRAM&&protocol==IPPROTO_UDP);++mock.opened;
    return mock.fail_open_at==mock.opened?-1:(int)mock.opened+99;
}
int llt_test_setsockopt(int fd,int level,int option,const void *value,socklen_t size)
{assert(fd>=100&&level==SOL_SOCKET&&option==SO_BINDTODEVICE&&size==sizeof(struct ifreq));
 const struct ifreq *iface=value;assert(!strcmp(iface->ifr_name,"ap"));return 0;}
int llt_test_bind(int fd,const struct sockaddr *address,socklen_t size)
{
    assert(fd>=100&&size==sizeof(struct sockaddr_in));const struct sockaddr_in *a=(const void *)address;
    assert(a->sin_family==AF_INET&&ntohl(a->sin_addr.s_addr)==mock.ap_ip);
    assert(ntohs(a->sin_port)==53||ntohs(a->sin_port)==123);return 0;
}
int llt_test_fcntl(int fd,int command,...){assert(fd>=100&&command==F_SETFL);return 0;}
int llt_test_close(int fd){assert(fd>=100);++mock.closed;return 0;}
ssize_t llt_test_recvfrom(int fd,void *out,size_t capacity,int flags,struct sockaddr *address,socklen_t *size)
{
    assert(fd>=100&&capacity==97&&flags==MSG_DONTWAIT);
    if(mock.recv_fail){errno=EBADF;return -1;}
    packet_t *packet=(fd%2==0)?&mock.dns_packet:&mock.ntp_packet;
    if(!packet->queued){errno=EAGAIN;return -1;}
    assert(*size==sizeof(struct sockaddr_in));struct sockaddr_in *a=(void *)address;
    *a=(struct sockaddr_in){.sin_family=AF_INET,.sin_port=htons(packet->port),.sin_addr.s_addr=htonl(packet->ip)};
    memcpy(out,packet->bytes,packet->size);packet->queued=false;++mock.received;return (ssize_t)packet->size;
}
ssize_t llt_test_sendto(int fd,const void *bytes,size_t size,int flags,const struct sockaddr *address,socklen_t address_size)
{
    assert(fd>=100&&size<=96&&!flags&&address_size==sizeof(struct sockaddr_in));
    const struct sockaddr_in *a=(const void *)address;assert(ntohl(a->sin_addr.s_addr)==0xc0a80402u);
    assert(a->sin_port);++mock.sent;mock.last_fd=fd;memcpy(mock.reply,bytes,size);mock.reply_size=size;
    return mock.send_fail?-1:(ssize_t)size;
}
static void queue(bool dns)
{
    packet_t *packet=dns?&mock.dns_packet:&mock.ntp_packet;memset(packet,0,sizeof(*packet));
    packet->queued=true;packet->ip=0xc0a80402u;packet->port=53210;
    if(dns) {
        static const uint8_t question[]={3,'n','t','p',3,'w','i','z',5,'w','o','r','l','d',0,0,1,0,1};
        packet->bytes[5]=1;memcpy(packet->bytes+12,question,sizeof(question));packet->size=12+sizeof(question);
    } else {packet->bytes[0]=0x23;packet->size=48;packet->bytes[47]=42;}
}
static void poll(llt_idf_t *s,uint64_t now){mock.now=now;llt_idf_poll(s,now);}
int main(void)
{
    reset();llt_idf_t s={0};llt_idf_stop(&s);assert(!mock.closed);
    llt_idf_init(&s,&ap,clock_read,&mock);assert(!mock.opened);poll(&s,0);
    if(CONFIG_LWIP_MAX_SOCKETS<18) {
        assert(s.status.state==LLT_SOCKET_BUDGET_REFUSED&&!mock.opened);
        poll(&s,30000);assert(!mock.opened);puts("local time adapter: insufficient socket budget refused");return 0;
    }
    assert(mock.opened==2&&s.status.state==LLT_READY);
    queue(true);queue(false);poll(&s,0);assert(mock.received==1&&mock.sent==1&&s.status.ntp_replies==1);
    assert(mock.reply_size==48&&mock.reply[31]==42&&mock.reply[0]==0x24);
    poll(&s,20);assert(mock.received==2&&mock.sent==2&&s.status.dns_replies==1);
    queue(false);poll(&s,40);assert(mock.sent==2&&s.status.rate_limited==1);
    queue(false);poll(&s,1020);assert(mock.sent==3);
    llt_idf_stop(&s);assert(mock.closed==2&&s.status.state==LLT_OFF);
    llt_idf_stop(&s);assert(mock.closed==2);poll(&s,1100);assert(mock.opened==4);

    reset();llt_idf_init(&s,&ap,clock_read,&mock);mock.trusted=false;queue(true);queue(false);
    poll(&s,0);poll(&s,20);assert(mock.sent==1&&s.status.dns_replies==1&&s.status.clock_refused==1);
    assert(s.status.state==LLT_WAIT_CLOCK);mock.trusted=true;queue(false);poll(&s,1020);
    assert(mock.sent==2&&s.status.state==LLT_READY);

    reset();llt_idf_init(&s,&ap,clock_read,&mock);mock.dns=0x08080808u;poll(&s,0);
    assert(s.status.state==LLT_DNS_CONFIGURATION_REFUSED&&!mock.opened);
    mock.dns=mock.ap_ip;poll(&s,1000);assert(!mock.opened);poll(&s,30000);assert(mock.opened==2);
    mock.dns=0x08080808u;poll(&s,31000);assert(mock.closed==2&&s.status.state==LLT_DNS_CONFIGURATION_REFUSED);

    reset();llt_idf_init(&s,&sta,clock_read,&mock);poll(&s,0);assert(s.status.state==LLT_WAIT_AP&&!mock.opened);
    reset();llt_idf_init(&s,&ap,clock_read,&mock);mock.dhcp=false;poll(&s,0);assert(!mock.opened);
    reset();llt_idf_init(&s,&ap,clock_read,&mock);mock.fail_open_at=2;poll(&s,0);
    assert(mock.opened==2&&mock.closed==1&&s.status.state==LLT_BIND_FAILED&&s.status.io_errors==1);
    poll(&s,29999);assert(mock.opened==2);mock.fail_open_at=0;poll(&s,30000);assert(mock.opened==4);

    reset();llt_idf_init(&s,&ap,clock_read,&mock);queue(false);mock.peer=false;poll(&s,0);
    assert(!mock.sent&&s.status.foreign_peer==1);
    mock.peer=true;mock.lose_lease_at=2;queue(false);poll(&s,1000);
    assert(!mock.sent&&s.status.foreign_peer==2); /* Lost between admission and final send. */
    reset();llt_idf_init(&s,&ap,clock_read,&mock);queue(false);mock.ntp_packet.ip=0x08080808u;poll(&s,0);
    assert(!mock.sent&&s.status.foreign_peer==1);
    queue(false);mock.ntp_packet.size=49;poll(&s,20);assert(!mock.sent&&s.status.malformed==1);
    queue(false);mock.ntp_packet.size=97;poll(&s,40);assert(!mock.sent&&s.status.malformed==2);

    reset();llt_idf_init(&s,&ap,clock_read,&mock);mock.send_fail=true;queue(false);poll(&s,0);
    assert(s.status.io_errors==1&&mock.closed==2&&s.status.state==LLT_BIND_FAILED&&!s.status.ntp_replies);
    reset();llt_idf_init(&s,&ap,clock_read,&mock);mock.recv_fail=true;poll(&s,0);
    assert(s.status.io_errors==1&&mock.closed==2&&mock.opened==2);poll(&s,20);assert(mock.opened==2);
    reset();llt_idf_init(&s,&ap,clock_read,&mock);poll(&s,0);mock.ap_ip=0xc0a80501u;poll(&s,1000);
    assert(mock.closed==2&&s.status.state==LLT_WAIT_AP);
    puts("local time adapter: AP binding/lease recheck, one-packet fairness, untrusted-clock refusal, backoff and close paths passed");
    return 0;
}
