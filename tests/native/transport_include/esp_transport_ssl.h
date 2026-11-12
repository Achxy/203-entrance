#pragma once
#include "esp_transport.h"
esp_transport_handle_t esp_transport_ssl_init(void);
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t t, esp_err_t (*attach)(void *));
void esp_transport_ssl_set_common_name(esp_transport_handle_t t, const char *name);
void esp_transport_ssl_set_keep_alive(esp_transport_handle_t t, esp_transport_keep_alive_t *cfg);
