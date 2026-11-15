#ifndef LIGHTING_LOCAL_TIME_IDF_H
#define LIGHTING_LOCAL_TIME_IDF_H
#include "lighting_local_time.h"
#include "esp_netif.h"

typedef struct {
    esp_netif_t *ap;
    bool (*wall_clock)(llt_clock_t *sample,void *context);
    void *context;
    int dns_fd, ntp_fd;
    uint32_t ap_ip, netmask;
    uint64_t retry_at_ms, check_at_ms;
    llt_rate_t dns_rate, ntp_rate;
    llt_status_t status;
    bool initialized, dns_turn;
} llt_idf_t;

/* Initializes only local memory. Poll owns both sockets on the lighting worker.
 * This never changes DHCP, DNS configuration, leases, routing or host Wi-Fi. */
void llt_idf_init(llt_idf_t *service,esp_netif_t *ap,
                  bool (*wall_clock)(llt_clock_t *,void *),void *context);
/* At most one datagram is consumed per call, alternating DNS/NTP fairly.
 * AP association plus DHCP lease are required again immediately before reply.
 * Open/error retry is at most once per30s. Call only under the runtime guard. */
void llt_idf_poll(llt_idf_t *service,uint64_t now_ms);
/* Called before runtime quiesce acknowledgment, and on failed activation.
 * No DHCP restoration is needed: this service never mutates its configuration. */
void llt_idf_stop(llt_idf_t *service);
#endif
