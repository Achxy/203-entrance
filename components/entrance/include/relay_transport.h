#pragma once

#include "esp_transport.h"
#include <stdbool.h>

/* One relay client. Resolve only with no live transport; DNS may block, so call
   before acquiring the OTA quiescence lease, then recheck permission to start. */
bool relay_transport_resolve(const char *host);
esp_transport_handle_t relay_transport_create(const char *headers);
/* Call only after the websocket client has stopped and been destroyed. */
void relay_transport_destroy(void);
bool relay_transport_redirected(void);
unsigned relay_transport_address_count(void);

