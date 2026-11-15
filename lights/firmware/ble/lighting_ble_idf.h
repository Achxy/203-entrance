#ifndef LIGHTING_BLE_IDF_H
#define LIGHTING_BLE_IDF_H

#include "lighting_ble_frame.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LB_LINKS 2u

typedef enum { LB_REQUEST_EVENT = 1, LB_DISCONNECTED_EVENT } lb_event_type_t;
typedef enum { LB_ROLE_UNPROVEN = 0, LB_ROLE_GUEST, LB_ROLE_OWNER } lb_proven_role_t;
typedef struct {
    lb_event_type_t type;
    uint8_t route[16];
    uint32_t id;
    uint8_t operation;
    uint64_t deadline_ms;
    size_t size;
    uint8_t bytes[LB_REQUEST_MAX];
} lb_event_t;

/* Owns the otherwise unused NimBLE host. NVS must already be initialized.
 * Explicit start only after service credentials and worker are ready. */
esp_err_t lb_transport_start(void);
/* Call from the same application worker that consumes events, never a BLE
 * callback. Stops advertisements/connections/host without releasing BT memory
 * irreversibly, so a later maintenance recovery can restart it. */
esp_err_t lb_transport_stop(void);
bool lb_transport_running(void);
/* Optional notification wake for the existing worker. Its task must outlive
 * the started transport. Index0 notifications carry no data, only wakeups. */
void lb_transport_set_worker(TaskHandle_t worker);

/* Poll at least every50ms from the existing application worker. It expires
 * assemblies, dispatch deadlines and indication acknowledgements. */
void lb_transport_poll(uint64_t now_ms);
/* Closed-route events precede requests. No background crypto is done here. */
bool lb_transport_take(lb_event_t *event, uint64_t now_ms);
esp_err_t lb_transport_reply(const uint8_t route[16], uint32_t id, uint16_t status,
                             const uint8_t *payload, size_t size, uint64_t now_ms);
/* Only the authenticated application adapter calls this, after grant checks.
 * Shared-password handshake completion alone is insufficient. */
void lb_transport_set_authorized(const uint8_t route[16], bool authorized);
/* Called only after a verified SRP client proof. At most one proven guest BLE
 * link is admitted, leaving the second slot for owner admission. False means
 * the link is disconnected and its secure context must be closed. */
bool lb_transport_set_proven_role(const uint8_t route[16], lb_proven_role_t role);
void lb_transport_disconnect(const uint8_t route[16]);

#endif
