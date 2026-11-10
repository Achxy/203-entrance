#pragma once
#include "core.h"
#include "cJSON.h"

typedef struct {
    char version[32];
    uint8_t hash[32];
    size_t size;
} release_manifest_t;

/* Flat, unique-key JSON prevents ambiguous authentication/release requests. */
cJSON *protocol_object(const char *bytes, size_t length);
bool protocol_manifest(const char *bytes, size_t length, release_manifest_t *out);
bool protocol_release_version_allowed(const char *version, const char *requested);
