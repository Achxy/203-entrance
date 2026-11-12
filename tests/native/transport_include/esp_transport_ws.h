#pragma once
#include "esp_transport.h"
typedef struct {
    const char *ws_path, *user_agent, *headers;
    bool propagate_control_frames;
} esp_transport_ws_config_t;
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent);
esp_err_t esp_transport_ws_set_config(esp_transport_handle_t t, const esp_transport_ws_config_t *cfg);
