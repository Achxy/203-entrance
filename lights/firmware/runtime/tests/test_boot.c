#include "lighting_runtime_boot.h"
#include "lighting_runtime_recovery.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    lrb_t gate;
    lrr_t recovery={0};
    lrb_init(&gate,true);
    assert(!gate.quarantined&&!gate.retry_used);
    assert(!lrb_release_once(&gate)); /* An ordinary boot has no bypass action. */
    assert(!lrb_cancel_release(&gate)&&!gate.quarantined);

    /* Previously committed configuration survives a crash, but even a due
     * memory retry cannot admit its worker until the local administrator acts. */
    lrb_init(&gate,false);
    lrr_failure(&recovery,0);
    assert(gate.quarantined&&!gate.retry_used);
    assert(!lrr_take(&recovery,60000,!gate.quarantined));
    assert(recovery.pending&&recovery.attempts==0);
    assert(!lrb_cancel_release(&gate));
    assert(lrb_release_once(&gate)&&!gate.quarantined&&gate.retry_used);
    assert(!lrb_release_once(&gate));
    assert(lrr_take(&recovery,60000,!gate.quarantined));
    assert(!lrr_take(&recovery,60000,!gate.quarantined));

    /* TLS may still consume memory after an accepted retry. This keeps the
     * same authorization, permits bounded recovery, and cannot release twice. */
    lrr_failure(&recovery,60000);
    assert(!gate.quarantined&&gate.retry_used);
    assert(!lrr_take(&recovery,119999,!gate.quarantined));
    assert(lrr_take(&recovery,120000,!gate.quarantined));
    lrr_success(&recovery);
    assert(!lrb_release_once(&gate));

    /* An OTA admission race before allocation restores a retry that did not
     * start. Repeated cancellations cannot manufacture additional releases. */
    lrb_init(&gate,false);
    assert(lrb_release_once(&gate));
    assert(lrb_cancel_release(&gate));
    assert(gate.quarantined&&!gate.retry_used);
    assert(!lrb_cancel_release(&gate));
    assert(lrb_release_once(&gate));
    assert(!lrb_release_once(&gate));

    /* Another panic is a new boot: the old explicit release is never inherited.
     * An intentional expected reboot does not require a persistent flash write. */
    lrb_init(&gate,false);
    assert(gate.quarantined&&!gate.retry_used);
    lrr_failure(&recovery,0);
    assert(!lrr_take(&recovery,UINT64_MAX,!gate.quarantined));
    lrb_init(&gate,true);
    assert(!gate.quarantined&&!gate.retry_used);
    puts("PASS abnormal-boot quarantine, one admin release, resource backoff and crash containment");
}
