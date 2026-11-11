#include "relay_frame.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    relay_frame_t f = {0};
    assert(relay_frame_feed(&f, 1, true, 7, 0, "{\"v", 3) == 0);
    assert(relay_frame_feed(&f, 1, true, 7, 3, "\":1}", 4) == 1);
    assert(!strcmp(f.bytes, "{\"v\":1}"));
    assert(relay_frame_feed(&f, 1, false, 3, 0, "{\"v", 3) == 0);
    assert(relay_frame_feed(&f, 9, true, 1, 0, "x", 1) == 0);
    assert(relay_frame_feed(&f, 0, true, 4, 0, "\":1}", 4) == 1);
    assert(!strcmp(f.bytes, "{\"v\":1}"));
    assert(relay_frame_feed(&f, 0, true, 1, 0, "x", 1) == -1);
    assert(relay_frame_feed(&f, 2, true, 1, 0, "x", 1) == -1);
    f = (relay_frame_t){0};
    assert(relay_frame_feed(&f, 1, true, 3, 0, "x", 1) == 0);
    assert(relay_frame_feed(&f, 1, true, 3, 2, "x", 1) == -1);
    assert(relay_frame_feed(&f, 1, true, 2049, 0, "x", 1) == -1);
    assert(relay_frame_feed(&f, 1, true, 3, 0, "x", 1) == -1);
    for (unsigned total = 1; total <= RELAY_INPUT_MAX; total++) {
        f = (relay_frame_t){0};
        for (unsigned at = 0; at < total; at++)
            assert(relay_frame_feed(&f, 1, true, total, at, "a", 1) == (at + 1 == total ? 1 : 0));
        assert(f.used == total && f.bytes[total] == 0);
    }
    puts("relay fragmentation: all sizes through 2048 bytes passed");
}
