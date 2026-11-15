#ifndef LLT_MOCK_IDF_H
#define LLT_MOCK_IDF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#ifndef CONFIG_LWIP_MAX_SOCKETS
#define CONFIG_LWIP_MAX_SOCKETS 20
#endif
#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 0x10000
#endif
#define ESP_OK 0
#define ESP_NETIF_DHCP_SERVER 2u
#define ESP_IPADDR_TYPE_V4 0u
typedef struct { int id; } esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { uint8_t mac[6];esp_ip4_addr_t ip; } esp_netif_pair_mac_ip_t;
typedef enum { ESP_NETIF_DNS_MAIN,ESP_NETIF_DNS_BACKUP } esp_netif_dns_type_t;
typedef struct { struct { union { esp_ip4_addr_t ip4; } u_addr;uint8_t type; } ip; } esp_netif_dns_info_t;
typedef enum { ESP_NETIF_DHCP_STOPPED,ESP_NETIF_DHCP_STARTED } esp_netif_dhcp_status_t;
typedef enum { WIFI_MODE_STA,WIFI_MODE_AP,WIFI_MODE_APSTA } wifi_mode_t;
typedef struct { int num;struct { uint8_t mac[6]; } sta[8]; } wifi_sta_list_t;
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *);
unsigned esp_netif_get_flags(esp_netif_t *);
bool esp_netif_is_netif_up(esp_netif_t *);
int esp_netif_get_ip_info(esp_netif_t *,esp_netif_ip_info_t *);
int esp_netif_dhcps_get_status(esp_netif_t *,esp_netif_dhcp_status_t *);
int esp_netif_get_dns_info(esp_netif_t *,esp_netif_dns_type_t,esp_netif_dns_info_t *);
int esp_netif_get_netif_impl_name(esp_netif_t *,char *);
int esp_netif_dhcps_get_clients_by_mac(esp_netif_t *,int,esp_netif_pair_mac_ip_t *);
int esp_wifi_get_mode(wifi_mode_t *);
int esp_wifi_ap_get_sta_list(wifi_sta_list_t *);
int64_t esp_timer_get_time(void);
int llt_test_socket(int,int,int);
int llt_test_setsockopt(int,int,int,const void *,socklen_t);
int llt_test_bind(int,const struct sockaddr *,socklen_t);
int llt_test_fcntl(int,int,...);
int llt_test_close(int);
ssize_t llt_test_recvfrom(int,void *,size_t,int,struct sockaddr *,socklen_t *);
ssize_t llt_test_sendto(int,const void *,size_t,int,const struct sockaddr *,socklen_t);
#define socket llt_test_socket
#define setsockopt llt_test_setsockopt
#define bind llt_test_bind
#define fcntl llt_test_fcntl
#define close llt_test_close
#define recvfrom llt_test_recvfrom
#define sendto llt_test_sendto
#endif
