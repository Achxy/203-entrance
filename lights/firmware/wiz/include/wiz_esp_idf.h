#ifndef WIZ_ESP_IDF_H
#define WIZ_ESP_IDF_H
#include "wiz_driver.h"
#include <esp_netif.h>

typedef struct {
    esp_netif_t *ap;
    int socket_fd;
    void *authorize_context;
    bool (*authorize)(void *, uint8_t, const uint8_t[16], const wiz_patch_t *, uint64_t);
} wiz_esp_idf_t;

/* Does not configure/start Wi-Fi, create an AP, join campus Wi-Fi, or transmit.
 * ap must be the existing WIFI_AP_DEF Wi-Fi AP interface with its DHCP server. */
wiz_error_t wiz_esp_idf_init(wiz_esp_idf_t *adapter, esp_netif_t *ap,
                            bool (*authorize)(void *, uint8_t, const uint8_t[16],
                                               const wiz_patch_t *, uint64_t),
                            void *authorize_context, wiz_io_t *io);
/* Association and DHCP information only; these are candidate peers, not
 * authenticated or identified WiZ bulbs. No broadcast probing or pairing. */
wiz_error_t wiz_esp_idf_peers(esp_netif_t *ap, wiz_peer_t *peers,
                             size_t capacity, size_t *count);
#endif
