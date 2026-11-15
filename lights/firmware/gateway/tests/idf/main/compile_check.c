#include "gateway_service.h"
#include "gateway_nvs.h"
#include <assert.h>

/* Compile/link-only fixture. This workflow does not flash a device. Even if
 * manually run, the invalid arguments cause no provisioning/network writes. */
void app_main(void)
{
    assert(lgs_nvs_open(NULL,NULL)==ESP_ERR_INVALID_ARG);
    assert(lgs_service_init(NULL,0,NULL,NULL,NULL)==LGS_STORAGE_IO);
    assert(!lgs_service_request(NULL,LGS_OWNER,NULL,0,0,NULL,0,NULL,NULL));
}
