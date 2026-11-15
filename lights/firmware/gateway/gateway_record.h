#ifndef LIGHTS_GATEWAY_RECORD_H
#define LIGHTS_GATEWAY_RECORD_H

#include "lighting_authority.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LGS_NAME_MAX 32u
#define LGS_RECORD_BYTES 3072u
#define LGS_RECORD_VERSION 1u
#define LGS_OWNER_PROFILE 0u
#define LGS_GUEST_PROFILE 1u

typedef struct { uint8_t salt[16], verifier[384]; } lgs_profile_t;
typedef struct {
    uint8_t mac[6];
    char name[LGS_NAME_MAX + 1];
    bool present;
    bool legacy_allowed;
} lgs_bulb_t;
typedef struct {
    uint64_t id;
    uint8_t token_hash[32];
    char name[LGS_NAME_MAX + 1];
} lgs_grant_t;
typedef struct {
    uint64_t generation;
    lgs_profile_t profiles[2];
    bool has_home_key;
    uint8_t home_key[16];
    lg_policy_t policy;
    lgs_bulb_t bulbs[LG_MAX_LIGHTS];
    lgs_grant_t grants[LG_MAX_GUESTS];
} lgs_record_t;

bool lgs_name_valid(const char *text);
bool lgs_utf8_valid(const uint8_t *text, size_t size);
bool lgs_record_valid(const lgs_record_t *record);
bool lgs_record_encode(const lgs_record_t *record, uint8_t out[LGS_RECORD_BYTES]);
bool lgs_record_decode(lgs_record_t *record, const uint8_t *data, size_t size);
uint8_t lgs_enabled_mask(const lgs_record_t *record);
void lgs_secure_zero(void *data, size_t size);

#endif
