/* Runs the production OTA state machine with deterministic flash/clock doubles.
 * Native RSA validity and true power loss require separate image/hardware tests. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../components/entrance/ota.c"

identity_t identity;
settings_t settings;
_Atomic bool settings_healthy;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
SemaphoreHandle_t operation_mutex = &mutex;
static esp_partition_t slots[2] = {{ENTRANCE_SLOT_BYTES, 0}, {ENTRANCE_SLOT_BYTES, 1}};
static uint8_t flash[2][ENTRANCE_SLOT_BYTES], candidate[8192];
static int running_slot, selected_slot, resets, confirmations, rollbacks, writes;
static int64_t clock_us;
static size_t write_at;
static esp_ota_img_states_t boot_state;
static update_record_t journal;
static esp_err_t load_error, store_error, signature_error, flash_error, activation_error;
static bool previous_valid, maintenance_called;
static esp_app_desc_t app;
#ifndef __APPLE__
size_t strlcpy(char *dst, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return len;
}
#endif
int psa_hash_setup(psa_hash_operation_t *o, int a)
{
    (void)a;
    o->value = 2166136261u;
    return 0;
}
int psa_hash_update(psa_hash_operation_t *o, const void *data, size_t n)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++)
        o->value = (o->value ^ p[i]) * 16777619u;
    return 0;
}
int psa_hash_finish(psa_hash_operation_t *o, uint8_t *out, size_t n, size_t *used)
{
    assert(n >= 32);
    for (int i = 0; i < 8; i++)
        memcpy(out + 4 * i, &o->value, 4);
    *used = 32;
    return 0;
}
int psa_hash_abort(psa_hash_operation_t *o)
{
    o->value = 0;
    return 0;
}
void sha256_bytes(const void *p, size_t n, uint8_t out[32])
{
    psa_hash_operation_t o;
    size_t used;
    psa_hash_setup(&o, 1);
    psa_hash_update(&o, p, n);
    psa_hash_finish(&o, out, 32, &used);
}
void random_hex(char *out, size_t n)
{
    memset(out, 'a', n * 2);
    out[n * 2] = 0;
}
void diag_event(const char *message)
{
    (void)message;
}
int64_t esp_timer_get_time(void)
{
    return clock_us;
}
void esp_restart(void)
{
    resets++;
}
void net_maintenance(void)
{
    maintenance_called = true;
}
const esp_app_desc_t *esp_app_get_description(void)
{
    return &app;
}
const esp_partition_t *esp_ota_get_running_partition(void)
{
    return &slots[running_slot];
}
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p)
{
    (void)p;
    return &slots[!running_slot];
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s)
{
    (void)p;
    *s = boot_state;
    return 0;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t at, void *out, size_t n)
{
    assert(at + n <= p->size);
    memcpy(out, flash[p->index] + at, n);
    return 0;
}
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t n, esp_ota_handle_t *h)
{
    assert(p->index != running_slot);
    assert(n <= p->size);
    write_at = 0;
    *h = p->index + 1;
    return 0;
}
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *p, size_t n)
{
    if (flash_error)
        return flash_error;
    assert(h - 1 != running_slot);
    assert(write_at + n <= ENTRANCE_SLOT_BYTES);
    memcpy(flash[h - 1] + write_at, p, n);
    write_at += n;
    writes++;
    return 0;
}
esp_err_t esp_ota_end(esp_ota_handle_t h)
{
    assert(h);
    return signature_error;
}
esp_err_t esp_ota_abort(esp_ota_handle_t h)
{
    assert(h);
    return 0;
}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    assert(!strcmp(journal.result, "armed"));
    if (activation_error)
        return activation_error;
    selected_slot = p->index;
    return 0;
}
esp_err_t esp_ota_check_image_validity(int t, const esp_image_header_t *h, const esp_app_desc_t *a)
{
    (void)t;
    (void)h;
    (void)a;
    return 0;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    confirmations++;
    boot_state = ESP_OTA_IMG_VALID;
    return 0;
}
esp_err_t esp_ota_mark_app_invalid_rollback(void)
{
    if (!previous_valid)
        return ESP_FAIL;
    rollbacks++;
    selected_slot = !running_slot;
    return 0;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{
    esp_err_t e = esp_ota_mark_app_invalid_rollback();
    if (!e)
        esp_restart();
    return e;
}
bool esp_ota_check_rollback_is_possible(void)
{
    return previous_valid;
}
esp_err_t store_update(update_record_t *r)
{
    if (store_error)
        return store_error;
    journal = *r;
    return 0;
}
esp_err_t load_update(update_record_t *r)
{
    if (load_error)
        return load_error;
    *r = journal;
    return 0;
}

static void ram_reset(void)
{
    writer = 0;
    destination = NULL;
    digest = (psa_hash_operation_t)PSA_HASH_OPERATION_INIT;
    memset(&record, 0, sizeof(record));
    received = 0;
    active = pending = reboot_scheduled = false;
    acknowledged = header_checked = baseline_healthy = false;
    last_activity = started = reboot_at = healthy_since = 0;
    expected_version[0] = 0;
    prefix_used = 0;
    strcpy(phase, "idle");
    clock_us = 1;
    settings_healthy = true;
}
static void fresh(void)
{
    ram_reset();
    memset(flash, 0xff, sizeof(flash));
    memset(&journal, 0, sizeof(journal));
    running_slot = selected_slot = 0;
    resets = confirmations = rollbacks = writes = 0;
    load_error = ESP_ERR_NVS_NOT_FOUND;
    store_error = signature_error = flash_error = activation_error = 0;
    boot_state = ESP_OTA_IMG_VALID;
    previous_valid = true;
    maintenance_called = false;
    memset(&app, 0, sizeof(app));
    strcpy(app.version, "1.0.0");
    memset(candidate, 0, sizeof(candidate));
    esp_image_header_t *h = (void *)candidate;
    h->magic = ESP_IMAGE_HEADER_MAGIC;
    h->chip_id = 5;
    esp_app_desc_t *a = (void *)(candidate + 32);
    a->magic_word = ESP_APP_DESC_MAGIC_WORD;
    strcpy(a->version, "1.1.0");
    strcpy(a->project_name, ENTRANCE_PROJECT);
    memcpy(candidate + 288, &entrance_descriptor, sizeof(entrance_descriptor));
    ota_init();
}
static void healthy(void)
{
    ota_tick(true);
    clock_us += 30000001;
    ota_tick(true);
}
static void begin(char ticket[65])
{
    uint8_t hash[32];
    sha256_bytes(candidate, sizeof(candidate), hash);
    assert(ota_begin(sizeof(candidate), hash, NULL, ticket) == ESP_OK);
}
static void transfer(char ticket[65])
{
    healthy();
    begin(ticket);
    assert(ota_write(0, candidate, sizeof(candidate)) == ESP_OK);
}
static void trial(void)
{
    running_slot = selected_slot;
    strcpy(app.version, "1.1.0");
    boot_state = ESP_OTA_IMG_PENDING_VERIFY;
    load_error = 0;
    ram_reset();
    ota_init();
    assert(ota_trial());
}
static void *concurrent_begin(void *out)
{
    uint8_t hash[32];
    char ticket[65];
    sha256_bytes(candidate, sizeof(candidate), hash);
    *(int *)out = ota_begin(sizeof(candidate), hash, NULL, ticket);
    return NULL;
}

int main(void)
{
    char ticket[65], second[65];
    uint8_t hash[32];
    fresh();
    sha256_bytes(candidate, sizeof(candidate), hash);
    assert(ota_begin(sizeof(candidate), hash, NULL, ticket) == ESP_ERR_INVALID_STATE);
    healthy();
    assert(ota_begin(ENTRANCE_SLOT_BYTES + 4096, hash, NULL, ticket) == ESP_ERR_INVALID_SIZE);
    assert(ota_begin(8193, hash, NULL, ticket) == ESP_ERR_INVALID_SIZE);
    begin(ticket);
    assert(ota_begin(sizeof(candidate), hash, NULL, second) == ESP_ERR_INVALID_STATE);
    assert(ota_write(1, candidate, 512) == ESP_ERR_INVALID_STATE);
    assert(writes == 0);
    assert(ota_finish() == ESP_ERR_INVALID_STATE);
    assert(selected_slot == 0);
    assert(ota_write(0, candidate, 8193) == ESP_ERR_INVALID_SIZE);
    clock_us += 30000001;
    ota_tick(true);
    assert(!ota_busy());
    assert(!strcmp(phase, "failed"));

    fresh();
    healthy();
    int outcomes[2];
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        assert(!pthread_create(&t[i], NULL, concurrent_begin, &outcomes[i]));
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);
    assert((outcomes[0] == 0) + (outcomes[1] == 0) == 1);
    ota_abort("test");

    for (int invalid = 0; invalid < 4; invalid++) {
        fresh();
        healthy();
        if (invalid == 0)
            ((esp_image_header_t *)candidate)->chip_id = 0;
        if (invalid == 1)
            strcpy(((esp_app_desc_t *)(candidate + 32))->version, "1.0.0");
        if (invalid == 2)
            ((entrance_descriptor_t *)(candidate + 288))->layout = 2;
        if (invalid == 3)
            strcpy(((esp_app_desc_t *)(candidate + 32))->version, "0.9.0");
        begin(ticket);
        assert(ota_write(0, candidate, sizeof(candidate)) != ESP_OK);
        assert(!ota_busy());
        assert(selected_slot == 0);
    }
    fresh();
    healthy();
    sha256_bytes(candidate, sizeof(candidate), hash);
    assert(!ota_begin(sizeof(candidate), hash, "1.2.0", ticket));
    assert(ota_write(0, candidate, sizeof(candidate)) != 0);
    fresh();
    healthy();
    begin(ticket);
    flash_error = ESP_FAIL;
    assert(ota_write(0, candidate, sizeof(candidate)) != 0);
    assert(!ota_busy());
    fresh();
    healthy();
    begin(ticket);
    candidate[500] ^= 1;
    assert(!ota_write(0, candidate, sizeof(candidate)));
    assert(ota_finish() == ESP_ERR_INVALID_CRC);
    assert(selected_slot == 0);
    fresh();
    transfer(ticket);
    signature_error = ESP_FAIL;
    assert(ota_finish() != 0);
    assert(selected_slot == 0);
    fresh();
    transfer(ticket);
    store_error = ESP_FAIL;
    assert(ota_finish() != 0);
    assert(selected_slot == 0);
    fresh();
    transfer(ticket);
    activation_error = ESP_FAIL;
    assert(ota_finish() != 0);
    assert(selected_slot == 0);

    fresh();
    transfer(ticket);
    assert(!ota_finish());
    assert(selected_slot == 1);
    trial();
    assert(ota_confirm("wrong") != 0);
    assert(!ota_confirm(ticket));
    assert(confirmations == 0);
    ota_tick(true);
    clock_us = 29000000;
    ota_tick(true);
    assert(confirmations == 0);
    ota_tick(false);
    clock_us = 30000000;
    ota_tick(true);
    clock_us = 59000000;
    ota_tick(true);
    assert(confirmations == 0);
    clock_us = 60000001;
    ota_tick(true);
    assert(confirmations == 1);
    assert(!ota_trial());
    assert(!strcmp(phase, "confirmed"));
    assert(!ota_confirm(ticket));
    assert(confirmations == 1);

    fresh();
    transfer(ticket);
    assert(!ota_finish());
    trial();
    healthy();
    assert(confirmations == 0);
    clock_us = 120000000;
    ota_tick(true);
    assert(rollbacks == 1 && resets == 1);
    fresh();
    transfer(ticket);
    assert(!ota_finish());
    trial();
    assert(!ota_confirm(ticket));
    clock_us = 120000000;
    ota_tick(false);
    assert(rollbacks == 1);
    fresh();
    transfer(ticket);
    assert(!ota_finish());
    trial();
    previous_valid = false;
    clock_us = 120000000;
    ota_tick(false);
    assert(maintenance_called && resets == 0);
    fresh();
    previous_valid = false;
    assert(ota_rollback() != 0);
    fresh();
    assert(!ota_rollback());
    assert(selected_slot == 1);

    fresh();
    transfer(ticket);
    assert(!ota_finish());
    running_slot = 1;
    load_error = 0;
    boot_state = ESP_OTA_IMG_VALID;
    ram_reset();
    ota_init();
    assert(!strcmp(phase, "confirmed")); /* reset after VALID, before journal confirmation */
    fresh();
    transfer(ticket);
    assert(!ota_finish());
    load_error = 0;
    ram_reset();
    ota_init();
    assert(!strcmp(phase, "rolled_back"));
    assert(ota_ticket_valid(ticket)); /* updater can observe its failed trial */
    fresh();
    transfer(ticket);
    assert(!ota_finish());
    journal.image_hash[0] ^= 1;
    trial();
    assert(!settings_healthy);
    fresh();
    load_error = ESP_ERR_INVALID_CRC;
    ram_reset();
    ota_init();
    assert(!settings_healthy);
    puts("OTA state tests passed: bounds, concurrent starts, offsets, timeouts, compatibility, "
         "digest/signature failure, journal/activation failures, health continuity, ticket, "
         "deadline, rollback, and crash windows");
    return 0;
}
