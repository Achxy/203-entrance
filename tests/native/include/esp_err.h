#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_INVALID_ARG 4
#define ESP_ERR_INVALID_VERSION 5
#define ESP_ERR_INVALID_CRC 6
#define ESP_ERR_NO_MEM 7
#define ESP_ERR_NVS_NOT_FOUND 8
#ifndef __APPLE__
size_t strlcpy(char *, const char *, size_t);
#endif
