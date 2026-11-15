#ifndef LIGHTS_GATEWAY_STORAGE_H
#define LIGHTS_GATEWAY_STORAGE_H
#include "gateway_record.h"

typedef enum {
    LGS_STORAGE_OK = 0, LGS_STORAGE_ABSENT, LGS_STORAGE_CORRUPT,
    LGS_STORAGE_IO, LGS_STORAGE_CONFLICT, LGS_STORAGE_MEMORY
} lgs_storage_result_t;

/* read_head returns ABSENT only when no head has ever been provisioned. The
 * integration must distinguish missing namespace from storage failure. */
typedef struct {
    void *context;
    lgs_storage_result_t (*read_head)(void *, uint64_t *generation);
    lgs_storage_result_t (*read_slot)(void *, unsigned slot, uint8_t *bytes, size_t *size);
    lgs_storage_result_t (*write_slot_commit)(void *, unsigned slot, const uint8_t *, size_t);
    lgs_storage_result_t (*write_head_commit)(void *, uint64_t generation);
} lgs_storage_t;

lgs_storage_result_t lgs_storage_load(const lgs_storage_t *storage, lgs_record_t *record);
/* Serialized compare/write/commit; failed writes never activate RAM policy.
 * A failed head commit is outcome-unknown: stop service until reload/recovery. */
lgs_storage_result_t lgs_storage_save(const lgs_storage_t *storage,
                                      uint64_t expected_generation,
                                      const lgs_record_t *record);

#endif
