#pragma once
#include "entrance.h"
/* Public relay is game-only. Configuration remains on the owner HTTPS API. */
esp_err_t relay_prepare(void);
esp_err_t relay_start(void);
esp_err_t relay_configure(const char *host, bool enabled);
bool relay_enabled(void);
/* Reserve memory for owner transfers; release each successful reservation. */
bool relay_quiesce(void);
void relay_resume(void);
cJSON *relay_status(void);
bool arcade_device_sign(const char *message, char signature[161]);
