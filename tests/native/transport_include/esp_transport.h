#pragma once
#include "esp_err.h"
typedef struct test_transport *esp_transport_handle_t;
typedef struct {
    bool keep_alive_enable;
    int keep_alive_idle, keep_alive_interval, keep_alive_count;
} esp_transport_keep_alive_t;
esp_err_t esp_transport_destroy(esp_transport_handle_t t);
esp_err_t esp_transport_set_default_port(esp_transport_handle_t t, int port);
