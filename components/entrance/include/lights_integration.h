#pragma once
#include "entrance.h"

/* Candidate-only adapter. Default builds retain the established display image. */
esp_err_t lights_prepare(void);
esp_err_t lights_start(void);
void lights_poll_recovery(uint64_t now_ms);
bool lights_quiesce(void);
void lights_resume(void);
bool lights_peak_begin(void);
void lights_peak_end(void);
bool lights_relay_ready(void);
bool lights_relay_message(const cJSON *message, bool *handled);
cJSON *lights_relay_next_reply(void);
void lights_relay_disconnected(void);
cJSON *lights_status(void);
esp_err_t lights_provision(const cJSON *request);
esp_err_t lights_retry(void);
