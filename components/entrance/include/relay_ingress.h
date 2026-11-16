#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RELAY_INGRESS_QUEUE_DEPTH 4u
#define RELAY_INGRESS_OWNERS (RELAY_INGRESS_QUEUE_DEPTH + 1u)
#define RELAY_INGRESS_SMALL_MAX 512u
#define RELAY_INGRESS_MAX 2048u
#define RELAY_INGRESS_FREE_RESERVE (32u * 1024u)
#define RELAY_INGRESS_BLOCK_RESERVE (24u * 1024u)
#define RELAY_INGRESS_ALLOCATION_MARGIN 1024u

typedef enum {
    RELAY_INGRESS_OK,
    RELAY_INGRESS_INVALID,
    RELAY_INGRESS_FULL,
    RELAY_INGRESS_PEAK_BUSY,
    RELAY_INGRESS_HEAP_GUARD,
    RELAY_INGRESS_ALLOCATION_FAILED,
    RELAY_INGRESS_QUEUE_FULL
} relay_ingress_result_t;

typedef struct {
    char *text;
    uint16_t length;
    uint8_t owner, heap;
} relay_ingress_item_t;

typedef struct {
    char small[RELAY_INGRESS_OWNERS][RELAY_INGRESS_SMALL_MAX + 1u];
    uint8_t used;
} relay_ingress_t;

typedef struct {
    /* Short critical sections protect only the five-bit ownership mask. */
    void (*lock)(void *context);
    void (*unlock)(void *context);
    /* OK retains the peak reservation; any refusal must release it itself. */
    relay_ingress_result_t (*begin_large)(size_t bytes, void *context);
    void (*end_large)(void *context);
    void *(*allocate)(size_t bytes, void *context);
    void (*deallocate)(void *memory, void *context);
    /* Both queue operations are nonblocking and copy only the descriptor. */
    bool (*enqueue)(const relay_ingress_item_t *item, void *context);
    bool (*dequeue)(relay_ingress_item_t *item, void *context);
    void *context;
} relay_ingress_ops_t;

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(relay_ingress_item_t) == 8, "C3 relay ingress descriptor size");
#endif

/* Initialize only before the producer starts, with no outstanding owners. */
void relay_ingress_init(relay_ingress_t *ingress);
bool relay_ingress_heap_admits(size_t bytes, size_t free_bytes, size_t largest_block);
/* Success transfers ownership to the queue. Refusal retains nothing. */
relay_ingress_result_t relay_ingress_submit(relay_ingress_t *ingress,
                                           const relay_ingress_ops_t *ops,
                                           const char *text, size_t length);
/* Exactly one consumer releases each transferred descriptor; clears it in place. */
void relay_ingress_release(relay_ingress_t *ingress, const relay_ingress_ops_t *ops,
                           relay_ingress_item_t *item);
/* Stop/destroy the producer before draining. Does not release a consumer's item. */
unsigned relay_ingress_drain(relay_ingress_t *ingress, const relay_ingress_ops_t *ops);
unsigned relay_ingress_owned(relay_ingress_t *ingress, const relay_ingress_ops_t *ops);
