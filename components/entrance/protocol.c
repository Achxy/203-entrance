#include "protocol.h"
#include <math.h>
#include <string.h>

cJSON *protocol_object(const char *bytes, size_t length)
{
    if (!bytes || !length || length > 4096 || memchr(bytes, 0, length))
        return NULL;
    /* cJSON's strings have no length, so disallow encoded NULs as well. */
    for (size_t i = 0; i + 6 <= length; i++)
        if (!memcmp(bytes + i, "\\u0000", 6))
            return NULL;
    const char *end = NULL;
    cJSON *j = cJSON_ParseWithLengthOpts(bytes, length, &end, false);
    if (!cJSON_IsObject(j))
        goto reject;
    while (end < bytes + length && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t'))
        end++;
    if (end != bytes + length)
        goto reject;
    size_t count = 0;
    for (cJSON *item = j->child; item; item = item->next) {
        if (++count > 16 || !item->string || cJSON_IsArray(item) || cJSON_IsObject(item))
            goto reject;
        for (cJSON *other = item->next; other; other = other->next)
            if (other->string && !strcmp(item->string, other->string))
                goto reject;
    }
    return j;
reject:
    cJSON_Delete(j);
    return NULL;
}

static bool integer(cJSON *j, const char *key, unsigned expected)
{
    cJSON *n = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsNumber(n) && n->valuedouble == expected;
}

bool protocol_manifest(const char *bytes, size_t length, release_manifest_t *out)
{
    cJSON *j = protocol_object(bytes, length);
    cJSON *version = cJSON_GetObjectItemCaseSensitive(j, "version");
    cJSON *size = cJSON_GetObjectItemCaseSensitive(j, "size");
    cJSON *hash = cJSON_GetObjectItemCaseSensitive(j, "sha256");
    cJSON *board = cJSON_GetObjectItemCaseSensitive(j, "board");
    bool valid = false;
    bool ok = j && cJSON_GetArraySize(j) == 7 && integer(j, "format", 1) &&
              integer(j, "layout", ENTRANCE_LAYOUT) && integer(j, "schema", ENTRANCE_SCHEMA) &&
              cJSON_IsString(version) && strlen(version->valuestring) < sizeof(out->version) &&
              cJSON_IsString(board) && !strcmp(board->valuestring, ENTRANCE_BOARD) &&
              cJSON_IsNumber(size) && isfinite(size->valuedouble) && size->valuedouble >= 8192 &&
              size->valuedouble <= ENTRANCE_SLOT_BYTES &&
              floor(size->valuedouble) == size->valuedouble &&
              (size_t)size->valuedouble % 4096 == 0 && cJSON_IsString(hash) &&
              hex_decode(hash->valuestring, out->hash, sizeof(out->hash));
    if (ok) {
        version_compare(version->valuestring, version->valuestring, &valid);
        ok = valid;
    }
    if (ok) {
        strcpy(out->version, version->valuestring);
        out->size = (size_t)size->valuedouble;
    }
    cJSON_Delete(j);
    return ok;
}

bool protocol_release_version_allowed(const char *version, const char *requested)
{
    if (!version)
        return false;
    bool valid = false;
    version_compare(version, version, &valid);
    if (!valid)
        return false;
    if (requested && *requested)
        return !strcmp(version, requested);
    const char *prerelease = strchr(version, '-'), *metadata = strchr(version, '+');
    return !prerelease || (metadata && prerelease > metadata);
}
