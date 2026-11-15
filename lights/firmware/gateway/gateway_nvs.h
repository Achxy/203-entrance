#ifndef LIGHTS_GATEWAY_NVS_H
#define LIGHTS_GATEWAY_NVS_H
#include "gateway_storage.h"
#include "nvs.h"
#include "esp_err.h"
typedef struct { nvs_handle_t handle; bool open; } lgs_nvs_t;
esp_err_t lgs_nvs_open(lgs_nvs_t *nvs, lgs_storage_t *storage);
void lgs_nvs_close(lgs_nvs_t *nvs);
#endif
