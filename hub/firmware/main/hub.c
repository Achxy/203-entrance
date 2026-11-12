#include "frame.h"
#include "driver/usb_serial_jtag.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdatomic.h>

static esp_netif_t *station;
static _Atomic bool has_ip, configured;
static _Atomic unsigned wifi_reason;
static int peer = -1;
static uint32_t connection;
static bool waiting_ack;
static uint64_t ack_at, contact_at, reconnect_at, byte_at;
static uint8_t tx[HUB_HEADER+HUB_MAX], frame[HUB_HEADER+HUB_MAX], network_data[HUB_MAX];
static size_t used;
static uint32_t corrupt_frames, bytes_up, bytes_down;
static uint64_t now_ms(void) { return esp_timer_get_time()/1000ULL; }

static bool emit(uint8_t type, uint32_t id, const void *p, size_t n)
{
    size_t total=hub_encode(tx,type,id,p,n);
    return total && usb_serial_jtag_write_bytes(tx,total,pdMS_TO_TICKS(500))==(int)total;
}
static void close_peer(void)
{
    if (peer>=0) { shutdown(peer,SHUT_RDWR); close(peer); peer=-1; }
    if (connection) emit(CLOSE,connection,NULL,0);
    connection=0; waiting_ack=false;
}
static void reply(uint8_t type, uint32_t id, bool ok, const char *reason)
{
    char json[160];
    int n=snprintf(json,sizeof(json),"{\"ok\":%s,\"reason\":\"%s\"}",ok?"true":"false",reason);
    emit(type|0x80,id,json,n);
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    (void)arg;
    if (base==WIFI_EVENT && event==WIFI_EVENT_STA_DISCONNECTED) {
        has_ip=false;
        wifi_reason=((wifi_event_sta_disconnected_t *)data)->reason;
    } else if (base==IP_EVENT && event==IP_EVENT_STA_GOT_IP) {
        has_ip=true; wifi_reason=0;
    }
}
static void status(uint32_t id)
{
    uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_STA,mac);
    esp_netif_ip_info_t ip={0}; esp_netif_get_ip_info(station,&ip);
    wifi_ap_record_t ap={0}; esp_wifi_sta_get_ap_info(&ap);
    char json[400];
    int n=snprintf(json,sizeof(json),
        "{\"protocol\":1,\"firmware\":\"1.0.0\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"connected\":%s,\"ip\":\"" IPSTR "\",\"rssi\":%d,\"wifi_reason\":%u,"
        "\"heap\":%lu,\"uptime_ms\":%llu,\"corrupt_frames\":%lu,\"bytes_up\":%lu,\"bytes_down\":%lu}",
        mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],has_ip?"true":"false",IP2STR(&ip.ip),
        ap.rssi,wifi_reason,(unsigned long)esp_get_free_heap_size(),now_ms(),
        (unsigned long)corrupt_frames,(unsigned long)bytes_up,(unsigned long)bytes_down);
    emit(HELLO|0x80,id,json,n);
}
static void join(uint32_t id, const uint8_t *p, size_t n)
{
    if (n<2 || p[0]<1 || p[0]>32 || p[1]>63 || n!=2u+p[0]+p[1] ||
        memchr(p+2,0,n-2)) { reply(JOIN,id,false,"invalid_credentials"); return; }
    close_peer(); configured=false; has_ip=false;
    esp_wifi_disconnect();
    wifi_config_t cfg={0};
    memcpy(cfg.sta.ssid,p+2,p[0]); memcpy(cfg.sta.password,p+2+p[0],p[1]);
    cfg.sta.scan_method=WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method=WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.failure_retry_cnt=2;
    cfg.sta.pmf_cfg.capable=true;
    esp_err_t e=esp_wifi_set_config(WIFI_IF_STA,&cfg);
    memset(&cfg,0,sizeof(cfg));
    if (e==ESP_OK) {
        configured=true; reconnect_at=now_ms()+10000;
        e=esp_wifi_connect();
    }
    reply(JOIN,id,e==ESP_OK,e==ESP_OK?"joining":"wifi_configuration_failed");
}
static void open_peer(uint32_t id,const uint8_t *p,size_t n)
{
    close_peer();
    if (!id || n!=6 || !has_ip) { reply(OPEN,id,false,"wifi_not_ready_or_invalid_target"); return; }
    struct sockaddr_in target={.sin_family=AF_INET};
    memcpy(&target.sin_addr.s_addr,p,4); memcpy(&target.sin_port,p+4,2);
    if (!target.sin_port || !target.sin_addr.s_addr) { reply(OPEN,id,false,"invalid_target"); return; }
    int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (fd<0) { reply(OPEN,id,false,"socket_allocation_failed"); return; }
    fcntl(fd,F_SETFL,O_NONBLOCK);
    int rc=connect(fd,(struct sockaddr *)&target,sizeof(target));
    if (rc<0 && errno==EINPROGRESS) {
        fd_set write_set; FD_ZERO(&write_set); FD_SET(fd,&write_set);
        struct timeval timeout={.tv_sec=4};
        rc=select(fd+1,NULL,&write_set,NULL,&timeout);
        int error=0; socklen_t size=sizeof(error);
        if (rc>0 && (getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)!=0 || error)) rc=-1;
    } else if (rc==0) rc=1;
    if (rc<=0) { close(fd); reply(OPEN,id,false,"tcp_connect_failed"); return; }
    int yes=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
    peer=fd; connection=id; waiting_ack=false;
    reply(OPEN,id,true,"connected");
}
static bool send_peer(const uint8_t *p,size_t n)
{
    uint64_t deadline=now_ms()+5000;
    while (n) {
        int sent=send(peer,p,n,0);
        if (sent>0) { p+=sent; n-=sent; bytes_up+=sent; continue; }
        if (sent<0 && (errno==EAGAIN || errno==EWOULDBLOCK) && now_ms()<deadline) {
            vTaskDelay(pdMS_TO_TICKS(10)); continue;
        }
        return false;
    }
    return true;
}
static void scan(uint32_t id,const uint8_t *p,size_t n)
{
    if (n>32 || (n && memchr(p,0,n))) { reply(SCAN,id,false,"invalid_ssid"); return; }
    close_peer(); configured=false; has_ip=false; esp_wifi_disconnect();
    uint8_t ssid[33]={0}; memcpy(ssid,p,n);
    wifi_scan_config_t cfg={.ssid=n?ssid:NULL,.show_hidden=true};
    if (esp_wifi_scan_start(&cfg,true)!=ESP_OK) { reply(SCAN,id,false,"scan_failed"); return; }
    wifi_ap_record_t aps[8]; uint16_t count=8;
    if (esp_wifi_scan_get_ap_records(&count,aps)!=ESP_OK) { reply(SCAN,id,false,"scan_results_failed"); return; }
    char json[HUB_MAX]; size_t at=snprintf(json,sizeof(json),"{\"ok\":true,\"aps\":[");
    for (unsigned i=0;i<count;i++) {
        char hex[65]; size_t length=strnlen((char *)aps[i].ssid,32);
        for (size_t j=0;j<length;j++) snprintf(hex+2*j,3,"%02x",aps[i].ssid[j]);
        hex[2*length]=0;
        uint8_t *m=aps[i].bssid;
        at+=snprintf(json+at,sizeof(json)-at,
            "%s{\"ssid_hex\":\"%s\",\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"channel\":%u,\"rssi\":%d,\"auth\":%d}",
            i?",":"",hex,m[0],m[1],m[2],m[3],m[4],m[5],aps[i].primary,aps[i].rssi,aps[i].authmode);
    }
    at+=snprintf(json+at,sizeof(json)-at,"]}");
    emit(SCAN|0x80,id,json,at);
}
static void dispatch(uint8_t kind,uint32_t id,uint8_t *p,size_t n)
{
    contact_at=now_ms();
    if (kind==HELLO && !n) status(id);
    else if (kind==JOIN) { join(id,p,n); memset(p,0,n); }
    else if (kind==OPEN) open_peer(id,p,n);
    else if (kind==SCAN) scan(id,p,n);
    else if (kind==DATA && id==connection && peer>=0 && n) {
        if (!send_peer(p,n) || !emit(ACK,id,NULL,0)) close_peer();
    } else if (kind==ACK && id==connection && !n) waiting_ack=false;
    else if (kind==CLOSE && id==connection && !n) close_peer();
}
static void consume(uint8_t b)
{
    byte_at=now_ms();
    if (used<4) {
        if (b==(uint8_t)"203H"[used]) frame[used++]=b;
        else used=(b=='2')?1:0;
        return;
    }
    frame[used++]=b;
    if (used<HUB_HEADER) return;
    size_t n=(size_t)frame[6] | (size_t)frame[7]<<8;
    if (frame[4]!=1 || n>HUB_MAX) { used=0; corrupt_frames++; close_peer(); return; }
    if (used<HUB_HEADER+n) return;
    uint32_t crc=hub_crc(hub_crc(0,frame,12),frame+HUB_HEADER,n);
    if (crc==hub_u32(frame+12)) dispatch(frame[5],hub_u32(frame+8),frame+HUB_HEADER,n);
    else { corrupt_frames++; close_peer(); }
    used=0;
}
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    station=esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT(); cfg.nvs_enable=0;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* The original Super Mini was hardware-qualified with this 8.5 dBm cap. */
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(34));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    usb_serial_jtag_driver_config_t usb={.tx_buffer_size=8192,.rx_buffer_size=8192};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    uint8_t input[1024];
    for (;;) {
        int n=usb_serial_jtag_read_bytes(input,sizeof(input),pdMS_TO_TICKS(10));
        for (int i=0;i<n;i++) consume(input[i]);
        uint64_t now=now_ms();
        if (used && now-byte_at>1500) { used=0; corrupt_frames++; close_peer(); }
        if (configured && !has_ip && now>=reconnect_at) {
            esp_wifi_connect(); reconnect_at=now+10000;
        }
        if (peer>=0 && (!has_ip || now-contact_at>20000 || (waiting_ack && now-ack_at>10000)))
            close_peer();
        if (peer>=0 && !waiting_ack) {
            n=recv(peer,network_data,sizeof(network_data),MSG_DONTWAIT);
            if (n>0) {
                if (emit(DATA,connection,network_data,n)) {
                    waiting_ack=true; ack_at=now; bytes_down+=n;
                } else close_peer();
            } else if (n==0 || (errno!=EAGAIN && errno!=EWOULDBLOCK)) close_peer();
        }
    }
}
