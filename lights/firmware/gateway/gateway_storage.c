#include "gateway_storage.h"
#include <stdlib.h>

static bool valid(const lgs_storage_t *s)
{
    return s && s->read_head && s->read_slot && s->write_slot_commit && s->write_head_commit;
}

lgs_storage_result_t lgs_storage_load(const lgs_storage_t *s, lgs_record_t *r)
{
    if (!valid(s) || !r) return LGS_STORAGE_IO;
    uint64_t head = 0;
    lgs_storage_result_t result = s->read_head(s->context, &head);
    if (result != LGS_STORAGE_OK) return result;
    if (!head) return LGS_STORAGE_CORRUPT;
    uint8_t *bytes = malloc(LGS_RECORD_BYTES);
    if (!bytes) return LGS_STORAGE_MEMORY;
    size_t size = LGS_RECORD_BYTES;
    result = s->read_slot(s->context, (unsigned)(head & 1u), bytes, &size);
    if (result == LGS_STORAGE_OK &&
        (!lgs_record_decode(r, bytes, size) || r->generation != head)) result = LGS_STORAGE_CORRUPT;
    if (result == LGS_STORAGE_ABSENT) result = LGS_STORAGE_CORRUPT;
    lgs_secure_zero(bytes, LGS_RECORD_BYTES); free(bytes);
    /* Never choose an older valid record: it may restore a revoked grant. */
    return result;
}

lgs_storage_result_t lgs_storage_save(const lgs_storage_t *s, uint64_t expected,
                                      const lgs_record_t *r)
{
    if (!valid(s) || !r || expected == UINT64_MAX || r->generation != expected+1)
        return LGS_STORAGE_CONFLICT;
    uint64_t current = 0;
    lgs_storage_result_t result = s->read_head(s->context, &current);
    if ((expected == 0 && result != LGS_STORAGE_ABSENT) ||
        (expected != 0 && (result != LGS_STORAGE_OK || current != expected)))
        return result == LGS_STORAGE_IO ? result : LGS_STORAGE_CONFLICT;
    uint8_t *bytes = malloc(LGS_RECORD_BYTES);
    if (!bytes) return LGS_STORAGE_MEMORY;
    if (!lgs_record_encode(r, bytes)) result = LGS_STORAGE_CORRUPT;
    else {
        result = s->write_slot_commit(s->context, (unsigned)(r->generation & 1u), bytes, LGS_RECORD_BYTES);
        if (result == LGS_STORAGE_OK) result = s->write_head_commit(s->context, r->generation);
    }
    lgs_secure_zero(bytes, LGS_RECORD_BYTES); free(bytes);
    return result;
}
