#include "gateway_nvs.h"

static lgs_storage_result_t result(esp_err_t e)
{
    return e == ESP_OK ? LGS_STORAGE_OK : e == ESP_ERR_NVS_NOT_FOUND ? LGS_STORAGE_ABSENT :
           e == ESP_ERR_NVS_INVALID_LENGTH ? LGS_STORAGE_CORRUPT : LGS_STORAGE_IO;
}
static lgs_storage_result_t read_head(void *ctx, uint64_t *generation)
{
    lgs_nvs_t *n = ctx;
    return result(nvs_get_u64(n->handle, "head", generation));
}
static lgs_storage_result_t read_slot(void *ctx, unsigned slot, uint8_t *bytes, size_t *size)
{
    lgs_nvs_t *n = ctx;
    return result(nvs_get_blob(n->handle, slot ? "record1" : "record0", bytes, size));
}
static lgs_storage_result_t write_slot(void *ctx, unsigned slot, const uint8_t *bytes, size_t size)
{
    lgs_nvs_t *n = ctx;
    nvs_stats_t stats;
    if (nvs_get_stats("state", &stats) != ESP_OK) return LGS_STORAGE_IO;
    /* Conservatively reserve the replacement blob, metadata/head and a full
     * 126-entry NVS page for garbage collection. Initial provisioning reserves
     * both copies. Never reclaim OTA records or stored media to make room. */
    uint64_t head = 0;
    esp_err_t current = nvs_get_u64(n->handle,"head",&head);
    if (current != ESP_OK && current != ESP_ERR_NVS_NOT_FOUND) return LGS_STORAGE_IO;
    size_t blob_entries = (LGS_RECORD_BYTES + 31u)/32u + 4u;
    size_t needed = blob_entries * (current == ESP_ERR_NVS_NOT_FOUND ? 2u : 1u) + 128u;
    if (stats.free_entries < needed) return LGS_STORAGE_IO;
    esp_err_t e = nvs_set_blob(n->handle, slot ? "record1" : "record0", bytes, size);
    if (e == ESP_OK) e = nvs_commit(n->handle);
    return result(e);
}
static lgs_storage_result_t write_head(void *ctx, uint64_t generation)
{
    lgs_nvs_t *n = ctx;
    esp_err_t e = nvs_set_u64(n->handle, "head", generation);
    if (e == ESP_OK) e = nvs_commit(n->handle);
    return result(e);
}
esp_err_t lgs_nvs_open(lgs_nvs_t *n, lgs_storage_t *storage)
{
    if (!n || !storage) return ESP_ERR_INVALID_ARG;
    *n = (lgs_nvs_t){0};
    esp_err_t e = nvs_open_from_partition("state", "lights_v1", NVS_READWRITE, &n->handle);
    if (e != ESP_OK) return e;
    n->open = true;
    *storage = (lgs_storage_t){n, read_head, read_slot, write_slot, write_head};
    return ESP_OK;
}
void lgs_nvs_close(lgs_nvs_t *n)
{
    if (!n || !n->open) return;
    nvs_close(n->handle); *n = (lgs_nvs_t){0};
}
