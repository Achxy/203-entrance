#include "relay_ingress.h"
#include <string.h>

void relay_ingress_init(relay_ingress_t *ingress)
{
    memset(ingress, 0, sizeof(*ingress));
}

bool relay_ingress_heap_admits(size_t bytes, size_t free_bytes, size_t largest_block)
{
    if (!bytes || bytes > RELAY_INGRESS_MAX + 1u)
        return false;
    size_t budget = bytes + RELAY_INGRESS_ALLOCATION_MARGIN;
    return free_bytes >= RELAY_INGRESS_FREE_RESERVE + budget &&
           largest_block >= RELAY_INGRESS_BLOCK_RESERVE + budget;
}

static uint8_t claim(relay_ingress_t *ingress, const relay_ingress_ops_t *ops)
{
    uint8_t owner = 0;
    ops->lock(ops->context);
    for (unsigned i = 0; i < RELAY_INGRESS_OWNERS; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!(ingress->used & bit)) {
            ingress->used |= bit;
            owner = (uint8_t)(i + 1u);
            break;
        }
    }
    ops->unlock(ops->context);
    return owner;
}

static void unclaim(relay_ingress_t *ingress, const relay_ingress_ops_t *ops, uint8_t owner)
{
    ops->lock(ops->context);
    ingress->used &= (uint8_t)~(1u << (owner - 1u));
    ops->unlock(ops->context);
}

void relay_ingress_release(relay_ingress_t *ingress, const relay_ingress_ops_t *ops,
                           relay_ingress_item_t *item)
{
    if (!item->text)
        return;
    /* Keep the owner reserved until clearing/freeing finishes: the producer
       must not reuse a slab while the previous consumer is still touching it. */
    memset(item->text, 0, (size_t)item->length + 1u);
    if (item->heap)
        ops->deallocate(item->text, ops->context);
    unclaim(ingress, ops, item->owner);
    memset(item, 0, sizeof(*item));
}

relay_ingress_result_t relay_ingress_submit(relay_ingress_t *ingress,
                                           const relay_ingress_ops_t *ops,
                                           const char *text, size_t length)
{
    if (!text || length > RELAY_INGRESS_MAX)
        return RELAY_INGRESS_INVALID;
    relay_ingress_item_t item = {.length = (uint16_t)length, .owner = claim(ingress, ops)};
    if (!item.owner)
        return RELAY_INGRESS_FULL;
    if (length <= RELAY_INGRESS_SMALL_MAX) {
        item.text = ingress->small[item.owner - 1u];
        memcpy(item.text, text, length);
        item.text[length] = 0;
    } else {
        relay_ingress_result_t result = ops->begin_large(length + 1u, ops->context);
        if (result != RELAY_INGRESS_OK) {
            unclaim(ingress, ops, item.owner);
            return result;
        }
        item.text = ops->allocate(length + 1u, ops->context);
        item.heap = 1;
        if (item.text) {
            memcpy(item.text, text, length);
            item.text[length] = 0;
        }
        ops->end_large(ops->context);
        if (!item.text) {
            unclaim(ingress, ops, item.owner);
            return RELAY_INGRESS_ALLOCATION_FAILED;
        }
    }
    if (!ops->enqueue(&item, ops->context)) {
        relay_ingress_release(ingress, ops, &item);
        return RELAY_INGRESS_QUEUE_FULL;
    }
    return RELAY_INGRESS_OK;
}

unsigned relay_ingress_drain(relay_ingress_t *ingress, const relay_ingress_ops_t *ops)
{
    relay_ingress_item_t item;
    unsigned drained = 0;
    while (drained < RELAY_INGRESS_QUEUE_DEPTH && ops->dequeue(&item, ops->context)) {
        relay_ingress_release(ingress, ops, &item);
        drained++;
    }
    return drained;
}

unsigned relay_ingress_owned(relay_ingress_t *ingress, const relay_ingress_ops_t *ops)
{
    ops->lock(ops->context);
    uint8_t used = ingress->used;
    ops->unlock(ops->context);
    unsigned count = 0;
    for (unsigned i = 0; i < RELAY_INGRESS_OWNERS; i++)
        count += (used >> i) & 1u;
    return count;
}
