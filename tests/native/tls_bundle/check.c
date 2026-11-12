#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/ecdsa.h"
#include "esp_crt_bundle.c"

const uint8_t test_bundle_start[1] asm("_binary_x509_crt_bundle_start") = {0};
const uint8_t test_bundle_end[1] asm("_binary_x509_crt_bundle_end") = {0};
static time_t test_time;
time_t time(time_t *p) { if (p) *p=test_time; return test_time; }

int main(int argc,char **argv)
{
    assert(argc==5);
    test_time=(time_t)strtoll(argv[4],NULL,10);
    assert(psa_crypto_init()==PSA_SUCCESS);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f); assert(n>0);
    uint8_t *bundle=malloc((size_t)n); assert(bundle);
    assert(fread(bundle,1,(size_t)n,f)==(size_t)n); fclose(f);
    assert(esp_crt_bundle_set(bundle,(size_t)n)==ESP_OK);
    mbedtls_ssl_config conf; mbedtls_ssl_config_init(&conf);
    assert(esp_crt_bundle_attach(&conf)==ESP_OK);
    mbedtls_x509_crt chain; mbedtls_x509_crt_init(&chain);
    assert(mbedtls_x509_crt_parse_file(&chain,argv[2])==0);
    uint32_t flags=0; int result;
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY
    result=mbedtls_x509_crt_verify_with_ca_cb(&chain,esp_crt_ca_cb_callback,NULL,
        &mbedtls_x509_crt_profile_default,argv[3],&flags,esp_crt_verify_callback,NULL);
#else
    result=mbedtls_x509_crt_verify(&chain,(mbedtls_x509_crt*)&s_dummy_crt,NULL,
        argv[3],&flags,esp_crt_verify_callback,NULL);
#endif
    printf("{\"result\":%d,\"flags\":%u}\n",result,(unsigned)flags);
    esp_crt_bundle_detach(&conf); mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&chain); free(bundle);
    return 0;
}
