#include "wiz_esp_idf.h"
#include <esp_wifi.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static wiz_error_t associated(esp_netif_t *ap, wifi_sta_list_t *stations,
                               esp_netif_ip_info_t *info)
{
    if (!ap || ap != esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) return WIZ_ARGUMENT;
    if (!esp_netif_is_netif_up(ap)) return WIZ_NO_PEER;
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK || (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA))
        return WIZ_NO_PEER;
    /* Fail closed on a caller handing in the station netif. */
    if (!(esp_netif_get_flags(ap) & ESP_NETIF_DHCP_SERVER)) return WIZ_ARGUMENT;
    if (esp_netif_get_ip_info(ap, info) != ESP_OK || esp_wifi_ap_get_sta_list(stations) != ESP_OK)
        return WIZ_NO_PEER;
    if (stations->num < 0 || stations->num > ESP_WIFI_MAX_CONN_NUM) return WIZ_IO;
    return WIZ_OK;
}

static wiz_error_t peer_for_mac(esp_netif_t *ap, const uint8_t mac[6], wiz_peer_t *peer)
{
    wifi_sta_list_t stations = {0};
    esp_netif_ip_info_t info;
    wiz_error_t e = associated(ap, &stations, &info);
    if (e != WIZ_OK) return e;
    bool found = false;
    for (int i = 0; i < stations.num; ++i)
        if (!memcmp(stations.sta[i].mac, mac, 6)) found = true;
    if (!found) return WIZ_NO_PEER;
    esp_netif_pair_mac_ip_t pair = {0};
    memcpy(pair.mac, mac, 6);
    if (esp_netif_dhcps_get_clients_by_mac(ap, 1, &pair) != ESP_OK || !pair.ip.addr) return WIZ_NO_PEER;
    memset(peer, 0, sizeof(*peer));
    peer->ap_ip = ntohl(info.ip.addr);
    peer->netmask = ntohl(info.netmask.addr);
    peer->ip = ntohl(pair.ip.addr);
    memcpy(peer->mac, mac, 6);
    return wiz_peer_allowed(peer, mac) ? WIZ_OK : WIZ_NO_PEER;
}

wiz_error_t wiz_esp_idf_peers(esp_netif_t *ap, wiz_peer_t *peers, size_t capacity, size_t *count)
{
    if (!peers || !count || !capacity || capacity > WIZ_MAX_BULBS) return WIZ_ARGUMENT;
    *count = 0;
    wifi_sta_list_t stations = {0};
    esp_netif_ip_info_t info;
    wiz_error_t e = associated(ap, &stations, &info);
    if (e != WIZ_OK) return e;
    for (int i = 0; i < stations.num; ++i) {
        wiz_peer_t peer;
        if (peer_for_mac(ap, stations.sta[i].mac, &peer) != WIZ_OK) continue;
        if (*count == capacity) return WIZ_TOO_LARGE;
        peers[(*count)++] = peer;
    }
    return WIZ_OK;
}

static wiz_error_t resolve(void *context, const uint8_t mac[6], wiz_peer_t *peer)
{
    wiz_esp_idf_t *adapter = context;
    return peer_for_mac(adapter->ap, mac, peer);
}

static void close_socket(void *context)
{
    wiz_esp_idf_t *adapter = context;
    if (adapter->socket_fd >= 0) close(adapter->socket_fd);
    adapter->socket_fd = -1;
}

static wiz_error_t open_socket(void *context, const wiz_peer_t *peer)
{
    wiz_esp_idf_t *adapter = context;
    close_socket(adapter);
    struct ifreq interface = {0};
    if (esp_netif_get_netif_impl_name(adapter->ap, interface.ifr_name) != ESP_OK) return WIZ_IO;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return WIZ_IO;
    adapter->socket_fd = fd;
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = 0, /* Fresh ephemeral port isolates delayed previous replies. */
        .sin_addr.s_addr = htonl(peer->ap_ip)
    };
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &interface, sizeof(interface)) != 0 ||
        bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        close_socket(adapter);
        return WIZ_IO;
    }
    return WIZ_OK;
}

static wiz_error_t send_datagram(void *context, const wiz_peer_t *peer, const uint8_t *data,
                                 size_t size, wiz_send_guard_t *guard)
{
    wiz_esp_idf_t *adapter = context;
    if (!guard || adapter->socket_fd < 0 || size > WIZ_DATAGRAM_MAX || !size) return WIZ_ARGUMENT;
    /* Resolve again immediately before the physical send, not just on poll. */
    wiz_peer_t current;
    wiz_error_t e = peer_for_mac(adapter->ap, peer->mac, &current);
    if (e != WIZ_OK) return e;
    if (current.ip != peer->ip || current.ap_ip != peer->ap_ip || current.netmask != peer->netmask)
        return WIZ_PEER_CHANGED;
    struct sockaddr_in remote = {
        .sin_family = AF_INET,
        .sin_port = htons(WIZ_PORT),
        .sin_addr.s_addr = htonl(peer->ip)
    };
    /* Peer/DHCP resolution may have consumed the last milliseconds of the
     * request. Recheck policy and its ORIGINAL deadline at the send boundary. */
    int64_t sampled_us = esp_timer_get_time();
    if (sampled_us < 0) return WIZ_CLOCK_ROLLBACK;
    e = wiz_send_guard_check(guard, (uint64_t)sampled_us / 1000);
    if (e != WIZ_OK) return e;
    ssize_t n = sendto(adapter->socket_fd, data, size, 0,
                       (struct sockaddr *)&remote, sizeof(remote));
    guard->transmitted = n >= 0 && (size_t)n == size;
    return guard->transmitted ? WIZ_OK : WIZ_IO;
}

static wiz_error_t receive_datagram(void *context, uint8_t *out, size_t capacity,
                                    size_t *size, uint32_t *ip, uint16_t *port)
{
    wiz_esp_idf_t *adapter = context;
    if (!out || !size || !ip || !port || capacity != WIZ_DATAGRAM_MAX + 1 || adapter->socket_fd < 0)
        return WIZ_ARGUMENT;
    *size = 0; *ip = 0; *port = 0;
    struct sockaddr_in source = {0};
    socklen_t source_size = sizeof(source);
    /* One extra byte detects oversized UDP packets even if lwIP truncates. */
    ssize_t n = recvfrom(adapter->socket_fd, out, capacity, MSG_DONTWAIT,
                         (struct sockaddr *)&source, &source_size);
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? WIZ_IDLE : WIZ_IO;
    if (source_size != sizeof(source) || source.sin_family != AF_INET) return WIZ_IO;
    *ip = ntohl(source.sin_addr.s_addr);
    *port = ntohs(source.sin_port);
    *size = (size_t)n;
    if ((size_t)n > WIZ_DATAGRAM_MAX) return WIZ_TOO_LARGE;
    return WIZ_OK;
}

static bool authorize(void *context, uint8_t index, const uint8_t id[16],
                       const wiz_patch_t *patch, uint64_t now)
{
    wiz_esp_idf_t *adapter = context;
    return adapter->authorize && adapter->authorize(adapter->authorize_context, index, id, patch, now);
}

wiz_error_t wiz_esp_idf_init(wiz_esp_idf_t *adapter, esp_netif_t *ap,
                            bool (*check)(void *, uint8_t, const uint8_t[16], const wiz_patch_t *, uint64_t),
                            void *check_context, wiz_io_t *io)
{
    if (!adapter || !ap || !io) return WIZ_ARGUMENT;
    memset(adapter, 0, sizeof(*adapter));
    adapter->ap = ap;
    adapter->socket_fd = -1;
    adapter->authorize = check;
    adapter->authorize_context = check_context;
    *io = (wiz_io_t){.context = adapter, .resolve = resolve, .open = open_socket,
        .send = send_datagram, .receive = receive_datagram, .close = close_socket,
        .authorize = authorize};
    return WIZ_OK;
}
