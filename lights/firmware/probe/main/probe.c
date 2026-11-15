/* Build/link resource probe only. Never deploy as production firmware. */
#include <assert.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#ifndef PROBE_ENTRY
#define PROBE_ENTRY app_main
#endif

#ifdef PROBE_INCLUDE_AUTHORITY
#include "lighting_authority.h"
static lg_core_t authority;
static lg_policy_t policy;
static lg_policy_stage_t policy_stage;
#endif

#ifdef PROBE_INCLUDE_GATEWAY
#include "gateway_service.h"
#include "wiz_esp_idf.h"
static lgs_service_t gateway_service;
static wiz_esp_idf_t gateway_adapter;
#endif

#ifdef PROBE_USE_TRANSPORT
#include "lighting_ble_idf.h"
static lb_event_t transport_event;
static volatile unsigned transport_api_probe;
static void retain_transport_api(void)
{
    uint8_t route[16] = {0};
    switch (transport_api_probe) {
    case 1: (void)lb_transport_stop(); break;
    case 2: (void)lb_transport_running(); break;
    case 3: lb_transport_poll(0); break;
    case 4: (void)lb_transport_take(&transport_event, 0); break;
    case 5: (void)lb_transport_reply(route, 0, 0, NULL, 0, 0); break;
    case 6: lb_transport_set_authorized(route, false); break;
    case 7: lb_transport_disconnect(route); break;
    case 8: lb_transport_set_worker(xTaskGetCurrentTaskHandle()); break;
    default: break;
    }
}
#endif

#if PROBE_STAGE >= 1
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

#if PROBE_STAGE >= 3
#include "lighting_session_idf.h"
static ls_manager_t manager;
static volatile unsigned session_api_probe;
static void retain_session_api(void)
{
    uint8_t route[16] = {0}, *reply = NULL;
    size_t size = 0;
    /* Volatile selector retains complete public paths in the link measurement.
       It remains zero; no real handshake or application request is attempted. */
    switch (session_api_probe) {
    case 1: (void)ls_manager_handshake(&manager, route, NULL, 0, 0, &reply, &size); break;
    case 2: (void)ls_manager_data(&manager, route, NULL, 0, 0, NULL, NULL, NULL, &reply, &size); break;
    case 3: ls_manager_expire(&manager, 0); break;
    case 4: ls_manager_close(&manager, route); break;
    case 5: ls_manager_close_guests(&manager); break;
    case 6: (void)ls_manager_bound_role(&manager, route); break;
    case 7: ls_manager_dispose(&manager); break;
    case 8: (void)ls_manager_proven_role(&manager, route); break;
    default: break;
    }
}
#endif

#if PROBE_STAGE >= 2
#include "protocomm.h"
#include "protocomm_security2.h"
static protocomm_t *contexts[2];
/* Public, deliberately invalid resource fixtures; no usable credentials. */
static const char salt[16] = {0};
static const char verifier[384] = {0};
static const protocomm_security2_params_t security = {
    .salt = salt, .salt_len = sizeof(salt),
    .verifier = verifier, .verifier_len = sizeof(verifier),
};
#endif

static void sample(const char *phase)
{
    ESP_LOGI("resource-probe", "%s free=%lu minimum=%lu largest=%u stack=%u",
             phase, (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

#if PROBE_STAGE >= 1 && !defined(PROBE_USE_TRANSPORT)
static const ble_uuid128_t service_id = BLE_UUID128_INIT(
    0x08,0x30,0x20,0x00,0x12,0x20,0x4c,0x90,0xaf,0x11,0x5c,0xc0,0xde,0x00,0x00,0x01);
static const ble_uuid128_t control_id = BLE_UUID128_INIT(
    0x08,0x30,0x20,0x00,0x12,0x20,0x4c,0x90,0xaf,0x11,0x5c,0xc0,0xde,0x00,0x00,0x02);

static int access_probe(uint16_t connection, uint16_t attribute,
                        struct ble_gatt_access_ctxt *context, void *argument)
{
    (void)connection; (void)attribute; (void)context; (void)argument;
    /* No unauthenticated control or provisioning exists in this probe. */
    return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
}

static const struct ble_gatt_svc_def services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &service_id.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &control_id.u, .access_cb = access_probe,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY },
          { 0 }
      } },
    { 0 }
};

static uint8_t address_type;
static unsigned connections;
static void advertise(void);
static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (!event->connect.status) connections++;
        if (connections < 2) advertise();
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        if (connections) connections--;
        advertise();
    }
    return 0;
}

static void advertise(void)
{
    const char *name = "203 Resource Probe";
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    assert(ble_gap_adv_set_fields(&fields) == 0);
    struct ble_gap_adv_params parameters = {0};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    parameters.itvl_min = 800;
    parameters.itvl_max = 960;
    assert(ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER,
                             &parameters, gap_event, NULL) == 0);
}

static void synced(void)
{
    assert(ble_hs_util_ensure_addr(0) == 0);
    assert(ble_hs_id_infer_auto(0, &address_type) == 0);
    advertise();
    sample("nimble-synced");
}

static void host_task(void *unused)
{
    (void)unused;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
#endif

void PROBE_ENTRY(void)
{
    sample("entry");
#ifdef PROBE_INCLUDE_GATEWAY
    /* Address escape keeps full objects in the ELF; no gateway runs here. */
    ESP_LOGI("resource-probe", "gateway=%u at=%p adapter=%u at=%p",
             (unsigned)sizeof(gateway_service), &gateway_service,
             (unsigned)sizeof(gateway_adapter), &gateway_adapter);
#endif
#ifdef PROBE_INCLUDE_AUTHORITY
    lg_policy_factory(&policy, 1);
    (void)lg_init(&authority, &policy, 1, 31);
    (void)lg_policy_stage_begin(&authority, (lg_identity_t){.session_id = LG_OWNER_SESSION},
                               &policy_stage, 0);
    ESP_LOGI("resource-probe", "authority=%u policy=%u policy-stage=%u",
             (unsigned)sizeof(authority), (unsigned)sizeof(policy), (unsigned)sizeof(policy_stage));
#endif
    /* Deliberately do not erase NVS on errors. No flashing is authorized. */
    ESP_ERROR_CHECK(nvs_flash_init());
    sample("nvs");
#if PROBE_STAGE >= 2
    for (unsigned i = 0; i < 2; i++) {
        contexts[i] = protocomm_new();
        assert(contexts[i]);
        ESP_ERROR_CHECK(protocomm_set_security(contexts[i], "security",
                                              &protocomm_security2, &security));
        ESP_ERROR_CHECK(protocomm_open_session(contexts[i], i + 1));
    }
    sample("two-security2-contexts-no-handshake");
#endif
#if PROBE_STAGE >= 3
    const uint8_t boot[8] = {0};
    ESP_ERROR_CHECK(ls_manager_init(&manager, "resource-probe", boot, &security, &security));
    ESP_LOGI("resource-probe", "session-manager-static-bytes=%u", (unsigned)sizeof(manager));
    retain_session_api();
#endif
#if defined(PROBE_USE_TRANSPORT)
    ESP_ERROR_CHECK(lb_transport_start());
    retain_transport_api();
#elif PROBE_STAGE >= 1
    ESP_ERROR_CHECK(nimble_port_init());
    sample("nimble-init");
    ble_svc_gap_init();
    ble_svc_gatt_init();
    assert(ble_gatts_count_cfg(services) == 0);
    assert(ble_gatts_add_svcs(services) == 0);
    assert(ble_svc_gap_device_name_set("203 Resource Probe") == 0);
    ble_hs_cfg.sync_cb = synced;
    nimble_port_freertos_init(host_task);
#endif
    for (;;) {
        sample("steady");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
