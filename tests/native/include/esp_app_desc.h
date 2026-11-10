#pragma once
#include <stdint.h>
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432u
typedef struct { uint32_t magic_word; uint8_t reserved[12]; char version[32],project_name[32]; uint8_t tail[176]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
