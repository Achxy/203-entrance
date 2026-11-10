#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ENTRANCE_MAGIC 0x32303345u
#define ENTRANCE_LAYOUT 1u
#define ENTRANCE_SCHEMA 1u
#define ENTRANCE_SLOT_BYTES 0x1E0000u
#define ENTRANCE_CHUNK 8192u
#define ENTRANCE_BOARD "esp32c3-supermini-4m"
#define ENTRANCE_PROJECT "203-entrance"
#define ENTRANCE_HEALTH_SECONDS 30
#define ENTRANCE_TRIAL_SECONDS 120

typedef struct {
    uint32_t schema, generation;
    char ssid[33], password[65];
    uint8_t enabled;
    uint8_t reserved[5];
    uint32_t crc;
} settings_t;
bool settings_valid(const settings_t *settings);
bool settings_select(const settings_t *a, bool read_a, const settings_t *b, bool read_b,
                     settings_t *out);

typedef struct {
    uint32_t magic;
    uint32_t layout;
    uint32_t schema_min;
    uint32_t schema_max;
    char board[32];
    uint32_t descriptor_version;
    uint32_t reserved[3];
} entrance_descriptor_t;

extern const entrance_descriptor_t entrance_descriptor;
int version_compare(const char *a, const char *b, bool *valid);
bool constant_equal(const void *a, const void *b, size_t len);
bool hex_decode(const char *text, uint8_t *output, size_t bytes);
void hex_encode(const void *data, size_t len, char *output);
uint32_t record_crc(const void *data, size_t len);
bool descriptor_compatible(const entrance_descriptor_t *descriptor);
