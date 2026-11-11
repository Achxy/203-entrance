#pragma once
#include "entrance.h"
#include "arcade_core.h"

#define ARCADE_PROFILE_MAX 640
#define ARCADE_RECEIPT_MAX 1536
#define ARCADE_GUEST_SSID "203 IoTitty"
#define ARCADE_GUEST_PASSWORD "neko-203-play"

esp_err_t arcade_prepare(void);
esp_err_t arcade_start(void);
bool arcade_healthy(void);
bool arcade_guest_network(void);
esp_err_t arcade_set_guest_network(bool enabled);
void arcade_maintenance(void);
void arcade_owner_release(void);
bool arcade_display_active(void);
bool arcade_frame_begin(void);
bool arcade_frame_row(unsigned width, unsigned height, unsigned y, uint16_t *row);
bool arcade_pair_row(unsigned y, uint16_t *row, unsigned width);
cJSON *arcade_status(void);
cJSON *arcade_logs(void);
cJSON *arcade_pairing(void);
bool arcade_profile_validate(const char *profile, cJSON **decoded);
bool arcade_receipt_sign(const char *payload, char out[ARCADE_RECEIPT_MAX]);
esp_err_t net_refresh_ap(void);

/* Both transports share admission, rate limits, logging and game rules. Body is flat strict JSON.
 */
cJSON *arcade_dispatch(const char *op, const char *token, const cJSON *body, uint32_t peer_id,
                       bool remote, int *status);
void arcade_relay_disconnected(void);
const char *arcade_boot_id(void);

cJSON *arcade_public_status(void);
