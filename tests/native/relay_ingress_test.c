#include "relay_ingress.h"
#include "relay_frame.h"
#include "protocol.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    void *memory;
    size_t bytes;
} allocation_t;
typedef struct {
    relay_ingress_t ingress;
    relay_ingress_ops_t ops;
    pthread_mutex_t owners_mutex, queue_mutex, allocations_mutex;
    relay_ingress_item_t queue[RELAY_INGRESS_QUEUE_DEPTH];
    unsigned head, queued, max_owners;
    allocation_t allocations[RELAY_INGRESS_OWNERS];
    size_t live_bytes, max_live_bytes, fake_free, fake_largest;
    unsigned allocate_calls, deallocate_calls, peak_calls, peak_acquired, peak_released;
    bool peak_busy, allocation_fail, enqueue_fail;
    atomic_bool producer_done;
    atomic_uint accepted, consumed;
} fixture_t;
static _Thread_local bool owner_lock_held, peak_held;

static void lock(void *context)
{
    fixture_t *f = context;
    assert(!owner_lock_held);
    assert(pthread_mutex_lock(&f->owners_mutex) == 0);
    owner_lock_held = true;
}
static void unlock(void *context)
{
    fixture_t *f = context;
    assert(owner_lock_held);
    unsigned count = 0;
    for (unsigned i = 0; i < RELAY_INGRESS_OWNERS; i++)
        count += (f->ingress.used >> i) & 1u;
    assert(count <= RELAY_INGRESS_OWNERS);
    if (count > f->max_owners)
        f->max_owners = count;
    owner_lock_held = false;
    assert(pthread_mutex_unlock(&f->owners_mutex) == 0);
}
static relay_ingress_result_t begin_large(size_t bytes, void *context)
{
    fixture_t *f = context;
    assert(!owner_lock_held && !peak_held);
    f->peak_calls++;
    if (f->peak_busy)
        return RELAY_INGRESS_PEAK_BUSY;
    f->peak_acquired++;
    assert(pthread_mutex_lock(&f->allocations_mutex) == 0);
    size_t live = f->live_bytes;
    assert(pthread_mutex_unlock(&f->allocations_mutex) == 0);
    if (live > f->fake_free || live > f->fake_largest ||
        !relay_ingress_heap_admits(bytes, f->fake_free - live, f->fake_largest - live)) {
        f->peak_released++;
        return RELAY_INGRESS_HEAP_GUARD;
    }
    peak_held = true;
    return RELAY_INGRESS_OK;
}
static void end_large(void *context)
{
    fixture_t *f = context;
    assert(!owner_lock_held && peak_held);
    f->peak_released++;
    peak_held = false;
}
static void *allocate(size_t bytes, void *context)
{
    fixture_t *f = context;
    assert(!owner_lock_held && peak_held);
    assert(bytes > RELAY_INGRESS_SMALL_MAX + 1u && bytes <= RELAY_INGRESS_MAX + 1u);
    f->allocate_calls++;
    if (f->allocation_fail)
        return NULL;
    void *memory = malloc(bytes);
    assert(memory);
    memset(memory, 0xa5, bytes);
    assert(pthread_mutex_lock(&f->allocations_mutex) == 0);
    unsigned slot = 0;
    while (slot < RELAY_INGRESS_OWNERS && f->allocations[slot].memory)
        slot++;
    assert(slot < RELAY_INGRESS_OWNERS);
    f->allocations[slot] = (allocation_t){memory, bytes};
    f->live_bytes += bytes;
    assert(f->live_bytes <= RELAY_INGRESS_OWNERS * (RELAY_INGRESS_MAX + 1u));
    if (f->live_bytes > f->max_live_bytes)
        f->max_live_bytes = f->live_bytes;
    assert(pthread_mutex_unlock(&f->allocations_mutex) == 0);
    return memory;
}
static void deallocate(void *memory, void *context)
{
    fixture_t *f = context;
    assert(!owner_lock_held && !peak_held);
    assert(pthread_mutex_lock(&f->allocations_mutex) == 0);
    unsigned slot = 0;
    while (slot < RELAY_INGRESS_OWNERS && f->allocations[slot].memory != memory)
        slot++;
    assert(slot < RELAY_INGRESS_OWNERS);
    size_t bytes = f->allocations[slot].bytes;
    for (size_t i = 0; i < bytes; i++)
        assert(((unsigned char *)memory)[i] == 0);
    f->live_bytes -= bytes;
    f->allocations[slot] = (allocation_t){0};
    f->deallocate_calls++;
    free(memory);
    assert(pthread_mutex_unlock(&f->allocations_mutex) == 0);
}
static bool enqueue(const relay_ingress_item_t *item, void *context)
{
    fixture_t *f = context;
    assert(!owner_lock_held && !peak_held);
    assert(item->owner >= 1 && item->owner <= RELAY_INGRESS_OWNERS);
    assert(item->text[item->length] == 0);
    assert(pthread_mutex_lock(&f->queue_mutex) == 0);
    bool ok = !f->enqueue_fail && f->queued < RELAY_INGRESS_QUEUE_DEPTH;
    if (ok) {
        f->queue[(f->head + f->queued) % RELAY_INGRESS_QUEUE_DEPTH] = *item;
        f->queued++;
    }
    assert(pthread_mutex_unlock(&f->queue_mutex) == 0);
    return ok;
}
static bool dequeue(relay_ingress_item_t *item, void *context)
{
    fixture_t *f = context;
    assert(!owner_lock_held && !peak_held);
    assert(pthread_mutex_lock(&f->queue_mutex) == 0);
    bool ok = f->queued != 0;
    if (ok) {
        *item = f->queue[f->head];
        f->queue[f->head] = (relay_ingress_item_t){0};
        f->head = (f->head + 1u) % RELAY_INGRESS_QUEUE_DEPTH;
        f->queued--;
    }
    assert(pthread_mutex_unlock(&f->queue_mutex) == 0);
    return ok;
}
static void initialize(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    assert(pthread_mutex_init(&f->owners_mutex, NULL) == 0);
    assert(pthread_mutex_init(&f->queue_mutex, NULL) == 0);
    assert(pthread_mutex_init(&f->allocations_mutex, NULL) == 0);
    f->ops = (relay_ingress_ops_t){lock, unlock, begin_large, end_large, allocate,
                                 deallocate, enqueue, dequeue, f};
    f->fake_free = 128u * 1024u;
    f->fake_largest = 96u * 1024u;
    atomic_init(&f->producer_done, false);
    atomic_init(&f->accepted, 0);
    atomic_init(&f->consumed, 0);
    relay_ingress_init(&f->ingress);
}
static void finish(fixture_t *f)
{
    relay_ingress_drain(&f->ingress, &f->ops);
    assert(!f->queued && !f->live_bytes);
    assert(!relay_ingress_owned(&f->ingress, &f->ops));
    assert(f->peak_acquired == f->peak_released);
    assert(!owner_lock_held && !peak_held);
    assert(pthread_mutex_destroy(&f->owners_mutex) == 0);
    assert(pthread_mutex_destroy(&f->queue_mutex) == 0);
    assert(pthread_mutex_destroy(&f->allocations_mutex) == 0);
}
static void payload(char *out, size_t size, uint32_t sequence)
{
    assert(size >= 4);
    for (size_t i = 0; i < size; i++)
        out[i] = (char)((sequence + i) % 251u + 1u);
    for (unsigned i = 0; i < 4; i++)
        out[i] = (char)(sequence >> (i * 8u));
}
static uint32_t verify(const relay_ingress_item_t *item)
{
    assert(item->length >= 4 && item->text[item->length] == 0);
    uint32_t sequence = 0;
    for (unsigned i = 0; i < 4; i++)
        sequence |= (uint32_t)(unsigned char)item->text[i] << (i * 8u);
    for (size_t i = 4; i < item->length; i++)
        assert((unsigned char)item->text[i] == (sequence + i) % 251u + 1u);
    return sequence;
}
static relay_ingress_result_t submit(fixture_t *f, size_t length, uint32_t sequence)
{
    char input[RELAY_INGRESS_MAX];
    payload(input, length, sequence);
    relay_ingress_result_t result = relay_ingress_submit(&f->ingress, &f->ops, input, length);
    memset(input, 0xcc, sizeof(input));
    return result;
}
static relay_ingress_item_t take(fixture_t *f, uint32_t sequence)
{
    relay_ingress_item_t item;
    assert(dequeue(&item, f));
    assert(verify(&item) == sequence);
    return item;
}
static void bounds(void)
{
    fixture_t f;
    initialize(&f);
    assert(relay_ingress_submit(&f.ingress, &f.ops, NULL, 0) == RELAY_INGRESS_INVALID);
    assert(relay_ingress_submit(&f.ingress, &f.ops, "x", RELAY_INGRESS_MAX + 1u) == RELAY_INGRESS_INVALID);
    const size_t sizes[] = {0, 1, 512, 513, 2048};
    char bytes[RELAY_INGRESS_MAX];
    memset(bytes, 'x', sizeof(bytes));
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        size_t n = sizes[i];
        assert(relay_ingress_submit(&f.ingress, &f.ops, bytes, n) == RELAY_INGRESS_OK);
        relay_ingress_item_t item;
        assert(dequeue(&item, &f));
        assert(item.length == n && item.text[n] == 0 && !memcmp(item.text, bytes, n));
        assert((item.heap != 0) == (n > RELAY_INGRESS_SMALL_MAX));
        relay_ingress_release(&f.ingress, &f.ops, &item);
        assert(!item.text && !item.owner && !item.length && !item.heap);
        relay_ingress_release(&f.ingress, &f.ops, &item);
    }
    assert(f.allocate_calls == 2 && f.deallocate_calls == 2);
    assert(!relay_ingress_heap_admits(0, SIZE_MAX, SIZE_MAX));
    assert(!relay_ingress_heap_admits(SIZE_MAX, SIZE_MAX, SIZE_MAX));
    assert(!relay_ingress_heap_admits(RELAY_INGRESS_MAX + 2u, SIZE_MAX, SIZE_MAX));
    for (size_t n = RELAY_INGRESS_SMALL_MAX + 2u; n <= RELAY_INGRESS_MAX + 1u; n++) {
        size_t free_bytes = RELAY_INGRESS_FREE_RESERVE + n + RELAY_INGRESS_ALLOCATION_MARGIN;
        size_t largest = RELAY_INGRESS_BLOCK_RESERVE + n + RELAY_INGRESS_ALLOCATION_MARGIN;
        assert(relay_ingress_heap_admits(n, free_bytes, largest));
        assert(!relay_ingress_heap_admits(n, free_bytes - 1u, largest));
        assert(!relay_ingress_heap_admits(n, free_bytes, largest - 1u));
    }
    finish(&f);
}
static void mixed_fifo(void)
{
    fixture_t f;
    initialize(&f);
    const size_t sizes[] = {64, 513, 512, 2048};
    for (unsigned i = 0; i < 4; i++)
        assert(submit(&f, sizes[i], i + 1u) == RELAY_INGRESS_OK);
    relay_ingress_item_t processing = take(&f, 1);
    assert(submit(&f, 100, 5) == RELAY_INGRESS_OK);
    assert(f.queued == 4 && relay_ingress_owned(&f.ingress, &f.ops) == 5);
    unsigned allocations = f.allocate_calls, peaks = f.peak_calls;
    assert(submit(&f, 2048, 6) == RELAY_INGRESS_FULL);
    assert(submit(&f, 32, 6) == RELAY_INGRESS_FULL);
    assert(f.allocate_calls == allocations && f.peak_calls == peaks);
    assert(verify(&processing) == 1);
    relay_ingress_release(&f.ingress, &f.ops, &processing);
    for (unsigned i = 2; i <= 5; i++) {
        relay_ingress_item_t item = take(&f, i);
        relay_ingress_release(&f.ingress, &f.ops, &item);
    }
    assert(f.max_owners == 5);
    finish(&f);
}
static void queue_full(void)
{
    fixture_t f;
    initialize(&f);
    for (unsigned i = 0; i < 4; i++)
        assert(submit(&f, 32, i + 1u) == RELAY_INGRESS_OK);
    assert(submit(&f, 32, 5) == RELAY_INGRESS_QUEUE_FULL);
    assert(submit(&f, 2048, 5) == RELAY_INGRESS_QUEUE_FULL);
    assert(relay_ingress_owned(&f.ingress, &f.ops) == 4 && !f.live_bytes);
    assert(f.allocate_calls == 1 && f.deallocate_calls == 1);
    assert(relay_ingress_drain(&f.ingress, &f.ops) == 4);
    f.enqueue_fail = true;
    assert(submit(&f, 512, 6) == RELAY_INGRESS_QUEUE_FULL);
    assert(submit(&f, 513, 7) == RELAY_INGRESS_QUEUE_FULL);
    assert(!relay_ingress_owned(&f.ingress, &f.ops) && !f.live_bytes);
    f.enqueue_fail = false;
    assert(submit(&f, 513, 8) == RELAY_INGRESS_OK);
    relay_ingress_item_t item = take(&f, 8);
    relay_ingress_release(&f.ingress, &f.ops, &item);
    finish(&f);
}
static void resource_refusal(void)
{
    fixture_t f;
    initialize(&f);
    f.peak_busy = true;
    assert(submit(&f, 513, 1) == RELAY_INGRESS_PEAK_BUSY);
    unsigned attempts = f.peak_calls;
    assert(submit(&f, 292, 2) == RELAY_INGRESS_OK);
    assert(submit(&f, 438, 3) == RELAY_INGRESS_OK);
    assert(f.peak_calls == attempts && !f.allocate_calls);
    assert(relay_ingress_drain(&f.ingress, &f.ops) == 2);
    f.peak_busy = false;
    f.fake_free = RELAY_INGRESS_FREE_RESERVE + 514u + RELAY_INGRESS_ALLOCATION_MARGIN - 1u;
    assert(submit(&f, 513, 4) == RELAY_INGRESS_HEAP_GUARD);
    f.fake_free = 128u * 1024u;
    f.fake_largest = RELAY_INGRESS_BLOCK_RESERVE + 514u + RELAY_INGRESS_ALLOCATION_MARGIN - 1u;
    assert(submit(&f, 513, 5) == RELAY_INGRESS_HEAP_GUARD);
    f.fake_largest = 96u * 1024u;
    f.allocation_fail = true;
    assert(submit(&f, 513, 6) == RELAY_INGRESS_ALLOCATION_FAILED);
    assert(!relay_ingress_owned(&f.ingress, &f.ops) && !f.live_bytes);
    f.allocation_fail = false;
    assert(submit(&f, 2048, 7) == RELAY_INGRESS_OK);
    relay_ingress_item_t item = take(&f, 7);
    relay_ingress_release(&f.ingress, &f.ops, &item);
    finish(&f);
}
static void maximum_burst(void)
{
    fixture_t f;
    initialize(&f);
    for (unsigned i = 1; i <= 4; i++)
        assert(submit(&f, RELAY_INGRESS_MAX, i) == RELAY_INGRESS_OK);
    relay_ingress_item_t processing = take(&f, 1);
    assert(submit(&f, RELAY_INGRESS_MAX, 5) == RELAY_INGRESS_OK);
    assert(f.live_bytes == RELAY_INGRESS_OWNERS * (RELAY_INGRESS_MAX + 1u));
    assert(submit(&f, RELAY_INGRESS_MAX, 6) == RELAY_INGRESS_FULL);
    assert(f.allocate_calls == 5 && f.max_owners == 5);
    assert(relay_ingress_drain(&f.ingress, &f.ops) == 4);
    assert(f.live_bytes == RELAY_INGRESS_MAX + 1u);
    assert(relay_ingress_owned(&f.ingress, &f.ops) == 1);
    assert(verify(&processing) == 1);
    relay_ingress_release(&f.ingress, &f.ops, &processing);
    finish(&f);
}
static void malformed_reconnect(void)
{
    fixture_t f;
    initialize(&f);
    relay_frame_t frame = {0};
    assert(relay_frame_feed(&frame, 1, true, 7, 0, "{\"v", 3) == 0);
    assert(relay_ingress_owned(&f.ingress, &f.ops) == 0);
    assert(relay_frame_feed(&frame, 1, true, 7, 4, "1}", 2) == -1);
    assert(relay_ingress_owned(&f.ingress, &f.ops) == 0);
    for (unsigned reconnect = 0; reconnect < 1000; reconnect++) {
        frame = (relay_frame_t){0};
        const char malformed[] = "{\"v\":1,\"v\":1}";
        assert(relay_frame_feed(&frame, 1, true, sizeof(malformed) - 1u, 0,
                                malformed, sizeof(malformed) - 1u) == 1);
        assert(relay_ingress_submit(&f.ingress, &f.ops, frame.bytes, frame.used) == RELAY_INGRESS_OK);
        for (unsigned i = 1; i <= 3; i++)
            assert(submit(&f, i & 1u ? 2048 : 512, i) == RELAY_INGRESS_OK);
        memset(&frame, 0, sizeof(frame));
        relay_ingress_item_t item;
        assert(dequeue(&item, &f));
        assert(!protocol_object(item.text, item.length));
        relay_ingress_release(&f.ingress, &f.ops, &item);
        assert(relay_ingress_drain(&f.ingress, &f.ops) == 3);
        assert(!relay_ingress_owned(&f.ingress, &f.ops) && !f.live_bytes);
        const char good[] = "{\"v\":1}";
        assert(relay_ingress_submit(&f.ingress, &f.ops, good, sizeof(good) - 1u) == RELAY_INGRESS_OK);
        assert(dequeue(&item, &f));
        cJSON *parsed = protocol_object(item.text, item.length);
        assert(parsed);
        cJSON_Delete(parsed);
        relay_ingress_release(&f.ingress, &f.ops, &item);
    }
    finish(&f);
}
static void *producer(void *context)
{
    fixture_t *f = context;
    for (uint32_t sequence = 1; sequence <= 20000; sequence++) {
        size_t length = sequence % 3u ? 4u + sequence % 509u : 513u + sequence % 1536u;
        relay_ingress_result_t result = submit(f, length, sequence);
        assert(result == RELAY_INGRESS_OK || result == RELAY_INGRESS_FULL ||
               result == RELAY_INGRESS_QUEUE_FULL);
        if (result == RELAY_INGRESS_OK)
            atomic_fetch_add(&f->accepted, 1);
        if (result != RELAY_INGRESS_OK || sequence % 4u == 0)
            sched_yield();
    }
    atomic_store(&f->producer_done, true);
    return NULL;
}
static void *consumer(void *context)
{
    fixture_t *f = context;
    uint32_t last = 0;
    for (;;) {
        relay_ingress_item_t item;
        if (dequeue(&item, f)) {
            uint32_t sequence = verify(&item);
            assert(sequence > last);
            last = sequence;
            if (sequence % 7u == 0)
                sched_yield();
            assert(verify(&item) == sequence);
            relay_ingress_release(&f->ingress, &f->ops, &item);
            atomic_fetch_add(&f->consumed, 1);
        } else if (atomic_load(&f->producer_done)) {
            /* Recheck after acquiring producer_done: its last enqueue may have
               followed the previous empty observation. */
            if (!dequeue(&item, f))
                break;
            assert(verify(&item) > last);
            last = verify(&item);
            relay_ingress_release(&f->ingress, &f->ops, &item);
            atomic_fetch_add(&f->consumed, 1);
        } else {
            sched_yield();
        }
    }
    return NULL;
}
static void concurrency(void)
{
    fixture_t f;
    initialize(&f);
    pthread_t sending, receiving;
    assert(pthread_create(&sending, NULL, producer, &f) == 0);
    assert(pthread_create(&receiving, NULL, consumer, &f) == 0);
    assert(pthread_join(sending, NULL) == 0);
    assert(pthread_join(receiving, NULL) == 0);
    assert(atomic_load(&f.accepted) == atomic_load(&f.consumed));
    assert(atomic_load(&f.accepted) > 0 && f.max_owners <= 5);
    finish(&f);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "bounds")) bounds();
    else if (!strcmp(argv[1], "mixed_fifo")) mixed_fifo();
    else if (!strcmp(argv[1], "queue_full")) queue_full();
    else if (!strcmp(argv[1], "resources")) resource_refusal();
    else if (!strcmp(argv[1], "maximum_burst")) maximum_burst();
    else if (!strcmp(argv[1], "malformed_reconnect")) malformed_reconnect();
    else if (!strcmp(argv[1], "concurrency")) concurrency();
    else assert(false);
    printf("relay owned ingress: %s passed\n", argv[1]);
    return 0;
}
