#pragma once
#include <stddef.h>
#include <stdint.h>
/* Test double for state/IO tests; this is NOT a cryptographic implementation. */
typedef struct { uint32_t value; } psa_hash_operation_t;
#define PSA_HASH_OPERATION_INIT {0}
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
int psa_hash_setup(psa_hash_operation_t *, int);
int psa_hash_update(psa_hash_operation_t *, const void *, size_t);
int psa_hash_finish(psa_hash_operation_t *, uint8_t *, size_t, size_t *);
int psa_hash_abort(psa_hash_operation_t *);
