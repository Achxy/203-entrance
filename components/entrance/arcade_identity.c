#include "arcade.h"
#include "relay.h"
#include "protocol.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "psa/crypto.h"
#include "esp_random.h"
#include <string.h>
#include <time.h>
#include <math.h>

extern const uint8_t arcade_pass_key[] asm("_binary_arcade_pass_public_hex_start");
extern const uint8_t arcade_pass_key_end[] asm("_binary_arcade_pass_public_hex_end");

static bool decode(const char *text, size_t length, uint8_t *out, size_t capacity, size_t *used)
{
    char padded[900];
    if (!length || length > sizeof(padded) - 5)
        return false;
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
        else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return false;
        padded[i] = c;
    }
    while (length % 4)
        padded[length++] = '=';
    return mbedtls_base64_decode(out, capacity, used, (uint8_t *)padded, length) == 0;
}
static bool encode(const uint8_t *data, size_t length, char *out, size_t capacity)
{
    size_t used;
    if (mbedtls_base64_encode((uint8_t *)out, capacity, &used, data, length))
        return false;
    while (used && out[used - 1] == '=')
        used--;
    out[used] = 0;
    for (size_t i = 0; i < used; i++) {
        if (out[i] == '+')
            out[i] = '-';
        if (out[i] == '/')
            out[i] = '_';
    }
    return true;
}
static bool text_field(const cJSON *j, const char *name, size_t limit, bool required)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, name);
    if (!cJSON_IsString(v) || strlen(v->valuestring) > limit || (required && !*v->valuestring))
        return false;
    bool visible = false;
    for (const unsigned char *p = (const unsigned char *)v->valuestring; *p; p++) {
        if (*p < 32 || *p == 127)
            return false;
        if (*p != ' ')
            visible = true;
    }
    return !required || visible;
}
bool arcade_profile_validate(const char *profile, cJSON **decoded)
{
    *decoded = NULL;
    if (!profile || strlen(profile) > ARCADE_PROFILE_MAX)
        return false;
    bool signed_pass = !strncmp(profile, "E1.", 3);
    if (!signed_pass && strncmp(profile, "G1.", 3))
        return false;
    const char *end = signed_pass ? strchr(profile + 3, '.') : profile + strlen(profile);
    if (!end)
        return false;
    uint8_t payload[513], signature[64], public_key[65];
    size_t used = 0, signature_size;
    if (!decode(profile + 3, end - profile - 3, payload, 512, &used))
        return false;
    payload[used] = 0;
    if (signed_pass) {
        if (!decode(end + 1, strlen(end + 1), signature, sizeof(signature), &signature_size) ||
            signature_size != 64 || arcade_pass_key_end - arcade_pass_key < 130)
            return false;
        char hex[131];
        memcpy(hex, arcade_pass_key, 130);
        hex[130] = 0;
        if (!hex_decode(hex, public_key, sizeof(public_key)))
            return false;
        char message[550];
        int length = snprintf(message, sizeof(message), "203-arcade-pass-v1\n%s", payload);
        if (length < 0 || (size_t)length >= sizeof(message))
            return false;
        uint8_t hash[32];
        sha256_bytes(message, length, hash);
        psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attr, 256);
        psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
        psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
        mbedtls_svc_key_id_t key = 0;
        psa_status_t status = psa_import_key(&attr, public_key, sizeof(public_key), &key);
        if (status == PSA_SUCCESS) {
            status = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, 32, signature, 64);
            psa_destroy_key(key);
        }
        psa_reset_key_attributes(&attr);
        if (status != PSA_SUCCESS)
            return false;
    }
    cJSON *j = protocol_object((char *)payload, used);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return false;
    }
    bool google = signed_pass && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "google"));
    bool valid = text_field(j, "name", 32, !google) && text_field(j, "room", 16, !google);
    if (signed_pass) {
        cJSON *version = cJSON_GetObjectItemCaseSensitive(j, "v");
        cJSON *expires = cJSON_GetObjectItemCaseSensitive(j, "exp");
        cJSON *issued = cJSON_GetObjectItemCaseSensitive(j, "iat");
        time_t now = time(NULL);
        valid = valid && cJSON_IsNumber(version) && version->valuedouble == 1 &&
                cJSON_IsNumber(expires) && isfinite(expires->valuedouble) &&
                cJSON_IsNumber(issued) && isfinite(issued->valuedouble) &&
                floor(expires->valuedouble) == expires->valuedouble &&
                floor(issued->valuedouble) == issued->valuedouble &&
                issued->valuedouble > 1700000000 && expires->valuedouble > issued->valuedouble &&
                expires->valuedouble - issued->valuedouble <= 28800;
        if (now > 1700000000)
            valid = valid && expires->valuedouble >= now && issued->valuedouble <= now + 60;
        if (google)
            valid = valid && text_field(j, "sub", 64, true);
    }
    if (!valid) {
        cJSON_Delete(j);
        return false;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(j, "google");
    cJSON_AddBoolToObject(j, "google", google);
    *decoded = j;
    return true;
}
bool arcade_receipt_sign(const char *payload, char out[ARCADE_RECEIPT_MAX])
{
    char message[1000];
    int length = snprintf(message, sizeof(message), "203-arcade-visit-v1\n%s", payload);
    if (length < 0 || (size_t)length >= sizeof(message))
        return false;
    uint8_t hash[32], sig[80];
    size_t siglen = 0;
    sha256_bytes(message, length, hash);
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    int err = mbedtls_pk_parse_key(&key, (const uint8_t *)identity.private_key,
                                   strlen(identity.private_key) + 1, NULL, 0);
    if (!err)
        err =
            mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sizeof(sig), &siglen);
    mbedtls_pk_free(&key);
    if (err)
        return false;
    memcpy(out, "R1.", 3);
    if (!encode((uint8_t *)payload, strlen(payload), out + 3, ARCADE_RECEIPT_MAX - 3))
        return false;
    size_t at = strlen(out);
    out[at++] = '.';
    return encode(sig, siglen, out + at, ARCADE_RECEIPT_MAX - at);
}

bool arcade_device_sign(const char *message, char signature[161])
{
    uint8_t hash[32], sig[80];
    size_t size = 0;
    sha256_bytes(message, strlen(message), hash);
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    int err = mbedtls_pk_parse_key(&key, (const uint8_t *)identity.private_key,
                                   strlen(identity.private_key) + 1, NULL, 0);
    if (!err)
        err = mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sizeof(sig), &size);
    mbedtls_pk_free(&key);
    if (err || size > 80)
        return false;
    hex_encode(sig, size, signature);
    return true;
}
