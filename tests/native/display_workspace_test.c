/* Exercise the actual display implementation with deterministic NVS/SPI doubles.
 * The game double borrows and overwrites exactly the supplied workspace; game
 * rendering itself has its independent sanitizer suite. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
static void *display_malloc(size_t);
static void display_free(void *);
#define malloc display_malloc
#define free display_free
#include "../../components/entrance/display.c"
#undef malloc
#undef free
#undef pixels
#if ENTRANCE_LIGHTS
#define test_pixels movie_storage.pixels
#else
#define test_pixels buffers.clip.pixels
#endif

identity_t identity;
settings_t settings;
_Atomic bool settings_healthy = true;
static pthread_mutex_t operation_lock = PTHREAD_MUTEX_INITIALIZER;
SemaphoreHandle_t operation_mutex = &operation_lock;
static int64_t test_clock;
static bool playing_game, fail_allocation, fail_commit, fail_read;
static unsigned allocation_count, commits, draws;
static size_t live_bytes, captured_size;
static uint8_t captured[160 * 128 * 2], expected[sizeof(captured)];
static uint8_t stored_bytes[MEDIA_MAX_BYTES], staged_bytes[MEDIA_MAX_BYTES];
static size_t stored_size, staged_size;
static display_config_t durable_config, staged_config;
static bool has_config, pending_clip, pending_config, pending_erase;
static struct { void *pointer; size_t size; } allocations[32];
#if !ENTRANCE_LIGHTS
static uint8_t game_workspace[ARCADE_FRAME_BYTES];
#endif
static uint8_t *game_workspace_pointer;

static void *display_malloc(size_t size)
{
    if (fail_allocation) { fail_allocation = false; return NULL; }
    void *pointer = malloc(size);
    assert(pointer);
    for (unsigned i = 0; i < 32; ++i) if (!allocations[i].pointer) {
        allocations[i].pointer = pointer; allocations[i].size = size;
        live_bytes += size; ++allocation_count; return pointer;
    }
    abort();
}
static void display_free(void *pointer)
{
    if (!pointer) return;
    for (unsigned i = 0; i < 32; ++i) if (allocations[i].pointer == pointer) {
        live_bytes -= allocations[i].size; allocations[i].pointer = NULL;
        free(pointer); return;
    }
    abort();
}
#ifndef __APPLE__
size_t strlcpy(char *dst, const char *src, size_t capacity)
{
    size_t size = strlen(src);
    if (capacity) { size_t n = size < capacity - 1 ? size : capacity - 1; memcpy(dst, src, n); dst[n] = 0; }
    return size;
}
#endif
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t m = malloc(sizeof(*m)); assert(m);
    assert(!pthread_mutex_init(m, NULL)); return m;
}
int xTaskCreate(void (*function)(void *), const char *name, size_t size, void *arg, unsigned priority, void *task)
{ (void)function; (void)name; (void)size; (void)arg; (void)priority; (void)task; return pdPASS; }
void vTaskDelay(unsigned ticks) { test_clock += (int64_t)ticks * 1000; }
int64_t esp_timer_get_time(void) { return test_clock; }
bool ota_busy(void) { return false; }
bool ota_trial(void) { return false; }
void diag_event(const char *message) { (void)message; }
void sha256_bytes(const void *data, size_t size, uint8_t out[32])
{
    uint32_t crc = record_crc(data, size);
    for (unsigned i = 0; i < 32; ++i) out[i] = (uint8_t)(crc >> ((i % 4) * 8));
}
void *spi_bus_dma_memory_alloc(int host, size_t size, unsigned flags)
{ (void)host; (void)flags; return malloc(size); }
esp_err_t spi_device_transmit(spi_device_handle_t device, spi_transaction_t *tx)
{
    (void)device; size_t size = tx->length / 8;
    assert(captured_size + size <= sizeof(captured));
    memcpy(captured + captured_size, tx->tx_buffer, size); captured_size += size;
    return ESP_OK;
}
bool arcade_display_active(void) { return playing_game; }
#if ENTRANCE_LIGHTS
bool arcade_frame_begin(uint8_t *workspace, size_t capacity)
{
    assert(workspace == test_pixels && capacity == ARCADE_FRAME_BYTES);
    game_workspace_pointer = workspace;
#else
bool arcade_frame_begin(void)
{
    game_workspace_pointer = game_workspace;
#endif
    if (playing_game) memset(game_workspace_pointer, 0xe7, ARCADE_FRAME_BYTES);
    return playing_game;
}
bool arcade_frame_row(unsigned width, unsigned height, unsigned y, uint16_t *row)
{
    assert(playing_game && y < height && game_workspace_pointer);
    for (unsigned x = 0; x < width; ++x)
        row[x] = (uint16_t)(0xf000 | game_workspace_pointer[(y * width + x) / 2]);
    return true;
}
bool arcade_pair_row(unsigned y, uint16_t *row, unsigned width)
{ (void)y; (void)row; (void)width; return false; }
esp_err_t nvs_open_from_partition(const char *partition, const char *space, int mode, nvs_handle_t *out)
{ assert(!strcmp(partition, "state") && !strcmp(space, "media_v1")); (void)mode; *out = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size)
{
    assert(handle == 1);
    if (fail_read) return ESP_FAIL;
    const void *source = NULL; size_t length = 0;
    if (!strcmp(key, "clip") && stored_size) { source = stored_bytes; length = stored_size; }
    else if (!strcmp(key, "config_v3") && has_config) { source = &durable_config; length = sizeof(durable_config); }
    else return ESP_ERR_NVS_NOT_FOUND;
    if (out && *size < length) return ESP_ERR_INVALID_SIZE;
    if (out) memcpy(out, source, length);
    *size = length; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size)
{
    assert(handle == 1);
    if (!strcmp(key, "clip")) {
        assert(size <= sizeof(staged_bytes)); memcpy(staged_bytes, data, size);
        staged_size = size; pending_clip = true;
    } else {
        assert(!strcmp(key, "config_v3") && size == sizeof(staged_config));
        memcpy(&staged_config, data, size); pending_config = true;
    }
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{ assert(handle == 1 && !strcmp(key, "clip")); pending_erase = true; return stored_size ? ESP_OK : ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1); ++commits;
    if (fail_commit) {
        fail_commit = false; pending_clip = pending_config = pending_erase = false;
        return ESP_FAIL;
    }
    if (pending_clip) { memcpy(stored_bytes, staged_bytes, staged_size); stored_size = staged_size; }
    if (pending_config) { durable_config = staged_config; has_config = true; }
    if (pending_erase) stored_size = 0;
    pending_clip = pending_config = pending_erase = false;
    return ESP_OK;
}

static esp_err_t configure(const char *json)
{
    cJSON *request = cJSON_Parse(json); assert(request);
    esp_err_t result = display_configure(request); cJSON_Delete(request); return result;
}
static void render(void)
{
    captured_size = 0; unsigned allocations_before = allocation_count;
    xSemaphoreTake(mutex, portMAX_DELAY);
    assert(draw() == ESP_OK);
    xSemaphoreGive(mutex);
    assert(captured_size == sizeof(captured));
    assert(allocation_count == allocations_before); ++draws;
}
static void interrupt_frame(void)
{
    render(); memcpy(expected, captured, sizeof(expected));
    uint32_t before_frame = movie.frame, before_loops = movie.loops;
    size_t before_offset = movie.offset, before_clip_offset = frame_offset;
    uint16_t before_clip_frame = frame;
    playing_game = true; render(); assert(memcmp(expected, captured, sizeof(expected)));
    playing_game = false; render(); assert(!memcmp(expected, captured, sizeof(expected)));
    if (replay_loaded) assert(movie.frame == before_frame && movie.loops == before_loops && movie.offset == before_offset);
    else assert(frame == before_clip_frame && frame_offset == before_clip_offset);
}
static void every_builtin_frame(void)
{
    for (unsigned slot = 1; slot <= 2; ++slot) {
        char request[80]; snprintf(request, sizeof(request), "{\"mode\":\"replay\",\"animation_slot\":%u}", slot);
        assert(configure(request) == ESP_OK);
#if ENTRANCE_LIGHTS
        assert(media == NULL && live_bytes == 0);
#endif
        for (unsigned f = 0; f < movie.info.frames; ++f) {
            assert(builtin_loop_seek(&movie, f));
            movie.loops = 11;
            for (unsigned rotation = 0; rotation < 4; ++rotation) {
                snprintf(request, sizeof(request), "{\"rotation\":%u}", rotation);
                assert(configure(request) == ESP_OK);
                for (unsigned caption = 0; caption < 3; ++caption) {
                    test_clock = (int64_t)caption_started_us + (int64_t)caption * 1350000;
                    interrupt_frame();
                }
            }
            /* A restored frame must also remain a valid base for the NEXT delta. */
            builtin_loop_t reference = movie;
            assert(builtin_loop_next(&reference));
            playing_game = true; render(); playing_game = false;
#if ENTRANCE_LIGHTS
            assert(restore_after_game());
#endif
            assert(builtin_loop_next(&movie));
            assert(!memcmp(movie.pixels, reference.pixels, sizeof(movie.pixels)));
            assert(movie.frame == reference.frame && movie.offset == reference.offset && movie.loops == reference.loops);
        }
    }
}
static void clip_lifecycle(void)
{
    size_t size = (size_t)(replay_asset_end - replay_asset_start);
    assert(display_install(replay_asset_start, size) == ESP_OK);
#if ENTRANCE_LIGHTS
    assert(media == NULL && live_bytes == 0);
    unsigned before_commits = commits;
    fail_allocation = true;
    assert(configure("{\"mode\":\"play\"}") == ESP_ERR_NO_MEM);
    assert(config.mode == MODE_REPLAY && media == NULL && commits == before_commits && live_bytes == 0);
    fail_commit = true;
    assert(configure("{\"mode\":\"play\"}") == ESP_FAIL);
    assert(config.mode == MODE_REPLAY && media == NULL && live_bytes == 0);
    fail_read = true;
    assert(configure("{\"mode\":\"play\"}") == ESP_ERR_INVALID_CRC);
    fail_read = false;
    assert(config.mode == MODE_REPLAY && media == NULL && live_bytes == 0);
#endif
    assert(configure("{\"mode\":\"play\",\"rotation\":1}") == ESP_OK);
#if ENTRANCE_LIGHTS
    assert(media && live_bytes == size);
#endif
    for (unsigned f = 0; f < info.frames; ++f) {
        if (f) { ++frame; assert(media_frame(media, media_size, info.width * info.height, &frame_offset, test_pixels)); }
        interrupt_frame();
        uint16_t retained_frame = frame; size_t retained_offset = frame_offset;
        render(); memcpy(expected, captured, sizeof(expected));
        assert(configure("{\"mode\":\"blank\"}") == ESP_OK);
#if ENTRANCE_LIGHTS
        assert(media == NULL && live_bytes == 0);
#endif
        playing_game = true; render(); playing_game = false; render();
        assert(configure("{\"mode\":\"pause\"}") == ESP_OK);
        assert(frame == retained_frame && frame_offset == retained_offset);
        render(); assert(!memcmp(expected, captured, sizeof(expected)));
    }
#if ENTRANCE_LIGHTS
    uint8_t replacement[MEDIA_MAX_BYTES];
    memcpy(replacement, replay_asset_start, size);
    replacement[20] = replacement[20] == 'A' ? 'B' : 'A';
    uint32_t crc = record_crc(replacement + 12, size - 12);
    for (unsigned i = 0; i < 4; ++i) replacement[8 + i] = (uint8_t)(crc >> (i * 8));
    assert(media_validate(replacement, size, NULL));
    before_commits = commits;
    uint8_t *retained_media = media;
    uint16_t retained_frame = frame;
    fail_allocation = true;
    assert(display_install(replacement, size) == ESP_ERR_NO_MEM);
    assert(media == retained_media && frame == retained_frame && commits == before_commits && live_bytes == size);
    fail_commit = true;
    assert(display_install(replacement, size) == ESP_FAIL);
    assert(media == retained_media && frame == retained_frame && live_bytes == size);
    interrupt_frame();
#endif
    size_t copy_size = 0; uint8_t *copy = display_copy(&copy_size);
    assert(copy && copy_size == size && !memcmp(copy, replay_asset_start, size)); display_free(copy);
    assert(configure("{\"mode\":\"replay_pause\",\"animation_slot\":2}") == ESP_OK);
    render(); copy = display_copy(&copy_size);
    assert(copy && copy_size == size && !memcmp(copy, replay_asset_start, size)); display_free(copy);
#if ENTRANCE_LIGHTS
    assert(media == NULL && live_bytes == 0);
#endif
    assert(display_remove() == ESP_OK && !media_size && !stored_clip);
    interrupt_frame();
    assert(live_bytes == 0);
}
int main(int argc, char **argv)
{
    if (argc == 2) {
        stored_size = (size_t)(replay_asset_end - replay_asset_start);
        memcpy(stored_bytes, replay_asset_start, stored_size);
        durable_config = config;
        durable_config.mode = !strcmp(argv[1], "clip") ? MODE_PAUSE : !strcmp(argv[1], "blank") ? MODE_BLANK : MODE_REPLAY;
        durable_config.crc = record_crc(&durable_config, offsetof(display_config_t, crc));
        has_config = true;
    }
    assert(display_start() == ESP_OK);
    if (argc == 2) {
        assert(config.mode == durable_config.mode && media_size == stored_size);
#if ENTRANCE_LIGHTS
        assert(live_bytes == (clip_mode(config.mode) ? stored_size : 0));
#endif
        render();
        assert(configure("{\"mode\":\"replay\"}") == ESP_OK);
    }
    assert(config.mode == MODE_REPLAY && replay_loaded);
    every_builtin_frame(); clip_lifecycle();
    assert(!pthread_mutex_destroy(mutex)); free(mutex); free(strip);
    printf("PASS display workspace: %u complete SPI frames, both animation slots, all orientations/captions, game/clip/blank transitions, no per-frame allocation; lights=%d\n", draws, ENTRANCE_LIGHTS);
    return 0;
}
