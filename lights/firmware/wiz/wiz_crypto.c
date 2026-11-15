#include "wiz_protocol.h"
#include <mbedtls/base64.h>
#include <psa/crypto.h>
#include <string.h>

void wiz_key_clear(wiz_key_t *key)
{
    if (!key) return;
    volatile uint8_t *p = (volatile uint8_t *)key;
    for (size_t n = 0; n < sizeof(*key); ++n) p[n] = 0;
}

wiz_error_t wiz_hmac_base64(const wiz_key_t *key, const uint8_t *data,
                           size_t size, char out[45])
{
    if (!key || !out || (!data && size) || size > WIZ_DATAGRAM_MAX)
        return WIZ_ARGUMENT;
    out[0] = 0;
    if (!key->present) return WIZ_KEY_REQUIRED;
    if (psa_crypto_init() != PSA_SUCCESS) return WIZ_CRYPTO_FAILED;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 128);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_status_t status = psa_import_key(&attributes, key->bytes, 16, &id);
    psa_reset_key_attributes(&attributes);
    uint8_t digest[32] = {0};
    size_t digest_size = 0, encoded_size = 0;
    if (status == PSA_SUCCESS)
        status = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), data,
                                 size, digest, sizeof(digest), &digest_size);
    if (!mbedtls_svc_key_id_is_null(id)) {
        psa_status_t destroy = psa_destroy_key(id);
        if (destroy != PSA_SUCCESS) status = destroy;
    }
    int encoded = -1;
    if (status == PSA_SUCCESS && digest_size == 32)
        encoded = mbedtls_base64_encode((unsigned char *)out, 45,
                                       &encoded_size, digest, 32);
    volatile uint8_t *p = digest;
    for (size_t n = 0; n < sizeof(digest); ++n) p[n] = 0;
    if (encoded != 0 || encoded_size != 44) {
        out[0] = 0;
        return WIZ_CRYPTO_FAILED;
    }
    out[44] = 0;
    return WIZ_OK;
}
