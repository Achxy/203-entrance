#include "relay_transport.h"
#include "esp_crt_bundle.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define RELAY_ADDRESS_LIMIT 4
static char hostname[128], address[INET_ADDRSTRLEN];
static uint32_t previous_address, selected_address;
static bool attempted, previous_ok, resolved;
static _Atomic unsigned address_count;
static _Atomic bool redirected;
/* Unrelated HTTP(S) transports can connect concurrently. Match exact handles,
   not hostnames, and publish/clear them before starting/after stopping the task. */
static _Atomic(esp_transport_handle_t) tls_transport, ws_transport;
static esp_transport_keep_alive_t keep_alive = {
    .keep_alive_enable = true, .keep_alive_idle = 5,
    .keep_alive_interval = 5, .keep_alive_count = 3,
};

bool relay_transport_resolve(const char *host)
{
    if (tls_transport || ws_transport)
        return false;
    resolved = false;
    address_count = 0;
    if (!host || !*host || strlen(host) >= sizeof(hostname))
        return false;
    if (strcmp(hostname, host)) {
        memcpy(hostname, host, strlen(host) + 1);
        attempted = false;
    }
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *result = NULL;
    if (getaddrinfo(hostname, "443", &hints, &result) != 0)
        return false;
    uint32_t candidates[RELAY_ADDRESS_LIMIT];
    unsigned count = 0;
    for (const struct addrinfo *p = result; p && count < RELAY_ADDRESS_LIMIT; p = p->ai_next) {
        if (p->ai_family != AF_INET || !p->ai_addr ||
            p->ai_addrlen < sizeof(struct sockaddr_in))
            continue;
        uint32_t ip = ((const struct sockaddr_in *)p->ai_addr)->sin_addr.s_addr;
        if (ip == INADDR_ANY || ip == INADDR_BROADCAST)
            continue;
        unsigned i = 0;
        while (i < count && candidates[i] != ip)
            i++;
        if (i == count)
            candidates[count++] = ip;
    }
    freeaddrinfo(result);
    if (!count)
        return false;
    /* Stable order prevents rotating DNS answers from repeatedly selecting the
       same failed endpoint. A successful endpoint remains preferred until its
       next failure or removal from DNS. Never retain an expired DNS address. */
    for (unsigned i = 1; i < count; i++) {
        uint32_t ip = candidates[i];
        unsigned j = i;
        while (j && ntohl(candidates[j - 1]) > ntohl(ip)) {
            candidates[j] = candidates[j - 1];
            j--;
        }
        candidates[j] = ip;
    }
    unsigned selected = 0;
    if (attempted) {
        for (unsigned i = 0; i < count; i++)
            if (candidates[i] == previous_address) {
                selected = previous_ok ? i : (i + 1) % count;
                break;
            }
    }
    struct in_addr ip = {.s_addr = candidates[selected]};
    if (!inet_ntop(AF_INET, &ip, address, sizeof(address)))
        return false;
    selected_address = ip.s_addr;
    address_count = count;
    resolved = true;
    return true;
}

esp_transport_handle_t relay_transport_create(const char *headers)
{
    if (!resolved || tls_transport || ws_transport)
        return NULL;
    esp_transport_handle_t tls = esp_transport_ssl_init();
    if (!tls)
        return NULL;
    esp_transport_ssl_crt_bundle_attach(tls, esp_crt_bundle_attach);
    /* ESP-TLS uses common_name both for SNI and certificate hostname matching.
       The socket dials an IP; the TLS identity and HTTP Host stay the DNS name. */
    esp_transport_ssl_set_common_name(tls, hostname);
    esp_transport_ssl_set_keep_alive(tls, &keep_alive);
    esp_transport_handle_t ws = esp_transport_ws_init(tls);
    esp_transport_ws_config_t cfg = {
        .ws_path = "/v1/device", .user_agent = "203-Arcade/1", .headers = headers,
        .propagate_control_frames = true,
    };
    if (!ws || esp_transport_set_default_port(ws, 443) != ESP_OK ||
        esp_transport_ws_set_config(ws, &cfg) != ESP_OK) {
        if (ws)
            esp_transport_destroy(ws);
        esp_transport_destroy(tls);
        return NULL;
    }
    redirected = false;
    tls_transport = tls;
    ws_transport = ws;
    return ws;
}

void relay_transport_destroy(void)
{
    esp_transport_handle_t ws = atomic_exchange(&ws_transport, NULL);
    esp_transport_handle_t tls = atomic_exchange(&tls_transport, NULL);
    if (ws)
        esp_transport_destroy(ws);
    if (tls)
        esp_transport_destroy(tls);
    resolved = false;
}

bool relay_transport_redirected(void) { return redirected; }
unsigned relay_transport_address_count(void) { return address_count; }

int __real_esp_transport_connect(esp_transport_handle_t t, const char *host, int port,
                                 int timeout_ms);
/* Linker wrapping is deliberately limited to our two external transport
   handles. The public SDK API offers no dial-address override separate from
   WS Host, nor a way to replace only an existing transport's connect callback.
   All other transports are passed through without altering their arguments. */
int __wrap_esp_transport_connect(esp_transport_handle_t t, const char *host, int port,
                                 int timeout_ms)
{
    esp_transport_handle_t tls = tls_transport, ws = ws_transport;
    if (!t || (t != tls && t != ws))
        return __real_esp_transport_connect(t, host, port, timeout_ms);
    if (!host || strcmp(host, hostname) || port != 443 || redirected) {
        redirected = true;
        errno = EACCES;
        return -1;
    }
    if (t == tls)
        return __real_esp_transport_connect(t, address, port, timeout_ms);
    /* Exactly one IP per normal retry/backoff; never multiply the TLS timeout
       or obstruct OTA quiescence by trying every address in one call. */
    attempted = true;
    previous_address = selected_address;
    int result = __real_esp_transport_connect(t, host, port, timeout_ms);
    previous_ok = result == 0;
    if (result > 0) {
        /* The SDK returns HTTP redirects to its caller. Reject before the
           websocket client can follow one with our signed admission headers. */
        redirected = true;
        errno = EACCES;
        return -1;
    }
    return result;
}
