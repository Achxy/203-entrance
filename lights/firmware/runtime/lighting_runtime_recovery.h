#ifndef LIGHTING_RUNTIME_RECOVERY_H
#define LIGHTING_RUNTIME_RECOVERY_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t next_ms;
    uint32_t failures, attempts;
    bool pending;
} lrr_t;

void lrr_failure(lrr_t *recovery, uint64_t now_ms);
void lrr_success(lrr_t *recovery);
bool lrr_due(const lrr_t *recovery, uint64_t now_ms, bool eligible);
bool lrr_take(lrr_t *recovery, uint64_t now_ms, bool eligible);
#endif
