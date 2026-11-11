#include "display.h"
#include "arcade.h"
#include "builtin_loop.h"
#include "captions.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The panel is write-only: SPI success cannot prove that pixels are visible. */
enum { MODE_TEST, MODE_PLAY, MODE_PAUSE, MODE_BLANK, MODE_REPLAY, MODE_REPLAY_PAUSE, MODE_COUNT };
static const char *modes[] = {"test", "play", "pause", "blank", "replay", "replay_pause"};
static const char *profiles[] = {"red", "green", "black"};
typedef struct {
    uint32_t magic;
    uint8_t mode, rotation, profile, inverted;
    uint32_t crc;
} display_config_t;
static display_config_t config = {.magic = 0x31444345, .rotation = 1, .profile = 2};
static nvs_handle_t storage;
static SemaphoreHandle_t mutex;
static spi_device_handle_t spi;
static union {
    struct {
        uint8_t media[MEDIA_MAX_BYTES], pixels[MEDIA_MAX_PIXELS / 2];
    } clip;
    struct {
        builtin_loop_t player;
    } builtin;
} buffers;
#define media buffers.clip.media
#define pixels buffers.clip.pixels
#define movie buffers.builtin.player
static bool replay_loaded;
extern const uint8_t replay_asset_start[] asm("_binary_neko_ecl_start");
extern const uint8_t replay_asset_end[] asm("_binary_neko_ecl_end");
static uint8_t *strip;
static size_t media_size, frame_offset = MEDIA_HEADER_BYTES;
static media_info_t info;
static uint16_t frame;
static uint32_t rendered, loops, late_frames;
static uint32_t last_frame_us, max_frame_us;
static bool dirty = true, panel_dirty = true;
static bool storage_ready;
static bool stored_clip;
static char last_error[96], media_sha[65];
static _Atomic bool driver_ready;
static _Atomic uint32_t heartbeat_ms;

static bool replay_mode(unsigned mode)
{
    return mode == MODE_REPLAY || mode == MODE_REPLAY_PAUSE;
}
static bool open_replay(void)
{
    replay_loaded = builtin_loop_open(&movie, replay_asset_start, replay_asset_end - replay_asset_start);
    return replay_loaded;
}
const uint8_t *display_replay_asset(size_t *size)
{
    *size = replay_asset_end - replay_asset_start;
    return replay_asset_start;
}
static esp_err_t send_bytes(bool data, const void *bytes, size_t length)
{
    gpio_set_level(1, data);
    spi_transaction_t t = {.length = length * 8, .tx_buffer = bytes};
    /* Poll only tiny commands. A complete frame spends about 41 ms on the
       8 MHz wire; spinning through its pixel strips starves the idle-priority
       HTTPS/password worker. DMA completion lets that worker and IDLE run
       while the display task waits, without changing the panel timing. */
    return length > 32 ? spi_device_transmit(spi, &t) : spi_device_polling_transmit(spi, &t);
}
static esp_err_t command(uint8_t code, const uint8_t *data, size_t length)
{
    esp_err_t err = send_bytes(false, &code, 1);
    return err == ESP_OK && length ? send_bytes(true, data, length) : err;
}
/* ST7735R initialization adapted from the supplied Adafruit library; see NOTICE. */
static esp_err_t panel_init(void)
{
    static const uint8_t init[] = {
        0xb1, 3,    1,    0x2c, 0x2d, 0xb2, 3,    1,    0x2c, 0x2d, 0xb3, 6,    1,    0x2c,
        0x2d, 1,    0x2c, 0x2d, 0xb4, 1,    7,    0xc0, 3,    0xa2, 2,    0x84, 0xc1, 1,
        0xc5, 0xc2, 2,    0x0a, 0,    0xc3, 2,    0x8a, 0x2a, 0xc4, 2,    0x8a, 0xee, 0xc5,
        1,    0x0e, 0x3a, 1,    5,    0xe0, 16,   2,    0x1c, 7,    0x12, 0x37, 0x32, 0x29,
        0x2d, 0x29, 0x25, 0x2b, 0x39, 0,    1,    3,    0x10, 0xe1, 16,   3,    0x1d, 7,
        6,    0x2e, 0x2c, 0x29, 0x2d, 0x2e, 0x2e, 0x37, 0x3f, 0,    0,    2,    0x10};
    gpio_set_level(0, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(0, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_err_t err = command(0x01, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    if (err == ESP_OK)
        err = command(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    for (size_t at = 0; err == ESP_OK && at < sizeof(init);) {
        uint8_t code = init[at++], count = init[at++];
        err = command(code, init + at, count);
        at += count;
    }
    if (err == ESP_OK)
        err = command(0x13, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (err == ESP_OK)
        err = command(0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    return err;
}
static esp_err_t window(unsigned x, unsigned y, unsigned w, unsigned h)
{
    if (config.profile == 1) {
        x += config.rotation & 1 ? 1 : 2;
        y += config.rotation & 1 ? 2 : 1;
    }
    uint8_t columns[] = {0, x, 0, x + w - 1}, rows[] = {0, y, 0, y + h - 1};
    esp_err_t err = command(0x2a, columns, sizeof(columns));
    if (err == ESP_OK)
        err = command(0x2b, rows, sizeof(rows));
    return err == ESP_OK ? command(0x2c, NULL, 0) : err;
}
static uint16_t test_color(unsigned x, unsigned y, unsigned w, unsigned h)
{
    /* White perimeter, distinct corners, red/green/blue bars, grayscale ramp. */
    if (!x || !y || x == w - 1 || y == h - 1)
        return 0xffff;
    if (x < 12 && y < 12)
        return 0xffe0;
    if (x >= w - 12 && y < 12)
        return 0xf81f;
    if (x < 12 && y >= h - 12)
        return 0x07ff;
    if (x >= w - 12 && y >= h - 12)
        return 0xffff;
    if (y < h * 2 / 3) {
        const uint16_t bars[] = {0xf800, 0x07e0, 0x001f};
        return bars[x * 3 / w];
    }
    unsigned gray = x * 31 / (w - 1);
    return (gray << 11) | ((gray * 2) << 5) | gray;
}
static esp_err_t draw(void)
{
    int64_t started = esp_timer_get_time();
    unsigned w = config.rotation & 1 ? 160 : 128, h = config.rotation & 1 ? 128 : 160;
    if (panel_dirty) {
        const uint8_t orientation[] = {0xc0, 0xa0, 0x00, 0x60};
        uint8_t madctl = orientation[config.rotation] | (config.profile == 2 ? 0 : 8);
        esp_err_t err = command(0x36, &madctl, 1);
        if (err == ESP_OK)
            err = command(config.inverted ? 0x21 : 0x20, NULL, 0);
        if (err != ESP_OK)
            return err;
        panel_dirty = false;
    }
    bool game_frame = arcade_frame_begin();
    bool builtin = replay_mode(config.mode) && replay_loaded;
    unsigned dw = w, dh = h;
    bool clip = media_size && (config.mode == MODE_PLAY || config.mode == MODE_PAUSE);
    if (clip) {
        if (w * info.height > h * info.width)
            dw = h * info.width / info.height;
        else
            dh = w * info.height / info.width;
        if (!dw)
            dw = 1;
        if (!dh)
            dh = 1;
    }
    unsigned left = (w - dw) / 2, top = (h - dh) / 2;
    uint16_t caption[160];
    for (unsigned y = 0; y < h; y += 4) {
        for (unsigned row = 0; row < 4; row++) {
            unsigned py = y + row;
            if (game_frame) arcade_frame_row(w, h, py, caption);
            if (!game_frame && builtin && !builtin_loop_row(&movie, w, h, py, 0, caption))
                return ESP_ERR_INVALID_SIZE;
            bool pairing = arcade_pair_row(py, caption, w);
            for (unsigned x = 0; x < w; x++) {
                uint16_t color = 0;
                if (game_frame || builtin || pairing)
                    color = caption[x];
                else if (config.mode == MODE_TEST)
                    color = test_color(x, py, w, h);
                else if (clip && x >= left && x < left + dw && py >= top && py < top + dh) {
                    unsigned sx = (x - left) * info.width / dw;
                    unsigned sy = (py - top) * info.height / dh;
                    uint8_t index = media_pixel(pixels, sy * info.width + sx);
                    color = media[48 + index * 2] | (uint16_t)media[49 + index * 2] << 8;
                }
                unsigned at = (row * w + x) * 2;
                strip[at] = color >> 8;
                strip[at + 1] = color;
            }
        }
        esp_err_t err = window(0, y, w, 4);
        if (err == ESP_OK)
            err = send_bytes(true, strip, w * 4 * 2);
        if (err != ESP_OK)
            return err;
        /* Pixel DMA already releases the CPU until completion. No additional
           scheduler tick is needed between strips. */
    }
    rendered++;
    last_frame_us = (uint32_t)(esp_timer_get_time() - started);
    if (last_frame_us > max_frame_us)
        max_frame_us = last_frame_us;
    return ESP_OK;
}
static void reset_frame(void)
{
    frame = 0;
    frame_offset = MEDIA_HEADER_BYTES;
    memset(pixels, 0, sizeof(pixels));
    if (media_size &&
        !media_frame(media, media_size, info.width * info.height, &frame_offset, pixels)) {
        media_size = 0;
        strlcpy(last_error, "Stored clip could not be decoded. Upload it again.",
                sizeof(last_error));
    }
    dirty = true;
}
static void display_task(void *arg)
{
    (void)arg;
    int64_t next = 0;
    bool was_suspended = false, was_game = false;
    for (;;) {
        heartbeat_ms = (uint32_t)(esp_timer_get_time() / 1000);
        bool suspended = ota_busy() && !ota_trial();
        if (!suspended && driver_ready) {
            xSemaphoreTake(mutex, portMAX_DELAY);
            int64_t now = esp_timer_get_time();
            bool builtin = replay_mode(config.mode) && replay_loaded;
            bool game_active = arcade_display_active();
            if (game_active != was_game) dirty = true;
            was_game = game_active;
            bool playing = game_active || (builtin ? config.mode == MODE_REPLAY
                                   : config.mode == MODE_PLAY && media_size && info.frames > 1);
            int64_t period = game_active ? 50000 : builtin ? movie.period_us : media_size ? 1000000 / info.fps : 100000;
            if (was_suspended || !next || dirty)
                next = now + period;
            bool advance = playing && now >= next;
            if (advance) {
                if (game_active) {
                    /* Game simulation owns its fixed-step clock. */
                } else if (builtin) {
                    if (!builtin_loop_next(&movie))
                        driver_ready = false;
                } else if (++frame == info.frames) {
                    loops++;
                    reset_frame();
                } else if (!media_frame(media, media_size, info.width * info.height, &frame_offset,
                                        pixels)) {
                    driver_ready = false;
                }
                /* Retain the intended cadence instead of accumulating each
                   scheduler delay. Never run an unbounded catch-up loop. */
                next += period;
                if (next < now)
                    next = now + period;
            }
            if (dirty || advance) {
                if (driver_ready && draw() != ESP_OK)
                    driver_ready = false;
                if (!driver_ready) {
                    strlcpy(last_error,
                            "Display decode or SPI failure; inspect diagnostics and restart.",
                            sizeof(last_error));
                    diag_event("Display decode or SPI failure");
                }
                dirty = false;
                if (playing && esp_timer_get_time() > next)
                    late_frames++;
            }
            xSemaphoreGive(mutex);
        }
        was_suspended = suspended;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool display_healthy(void)
{
    return driver_ready && (uint32_t)(esp_timer_get_time() / 1000) - heartbeat_ms < 5000;
}
static bool valid_config(const display_config_t *value)
{
    return value->magic == 0x31444345 && value->mode < MODE_COUNT && value->rotation < 4 &&
           value->profile < 3 && value->inverted < 2 &&
           value->crc == record_crc(value, offsetof(display_config_t, crc));
}
esp_err_t display_start(void)
{
    mutex = xSemaphoreCreateMutex();
    if (!mutex)
        return ESP_ERR_NO_MEM;
    esp_err_t err = nvs_open_from_partition("state", "media_v1", NVS_READWRITE, &storage);
    storage_ready = err == ESP_OK;
    if (storage_ready) {
        display_config_t saved;
        size_t n = sizeof(saved);
        err = nvs_get_blob(storage, "config_v2", &saved, &n);
        bool legacy = err == ESP_ERR_NVS_NOT_FOUND;
        if (legacy) {
            n = sizeof(saved);
            err = nvs_get_blob(storage, "config", &saved, &n);
        }
        bool saved_valid = err == ESP_OK && n == sizeof(saved) && valid_config(&saved);
        if (saved_valid)
            config = saved;
        else if (err != ESP_ERR_NVS_NOT_FOUND)
            strlcpy(last_error,
                    "Display settings unreadable; using test card. Reapply panel settings.",
                    sizeof(last_error));
        if (legacy && (saved_valid || err == ESP_ERR_NVS_NOT_FOUND)) {
            config.mode = MODE_REPLAY;
            config.rotation = 1;
        }
        n = sizeof(media);
        err = nvs_get_blob(storage, "clip", media, &n);
        stored_clip = err != ESP_ERR_NVS_NOT_FOUND;
        if (err == ESP_OK && media_validate(media, n, &info)) {
            media_size = n;
            uint8_t hash[32];
            sha256_bytes(media, n, hash);
            hex_encode(hash, 32, media_sha);
            if (!replay_mode(config.mode))
                reset_frame();
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            strlcpy(last_error, "Stored clip unreadable; replace or remove it explicitly.",
                    sizeof(last_error));
        }
    } else {
        strlcpy(last_error, "Media storage unavailable. USB recovery is required.",
                sizeof(last_error));
    }
    if (!open_replay()) {
        strlcpy(last_error, "Embedded replay failed validation.", sizeof(last_error));
        return ESP_ERR_INVALID_CRC;
    }
    if (!replay_mode(config.mode)) {
        replay_loaded = false;
        if (media_size) {
            size_t length = sizeof(media);
            if (nvs_get_blob(storage, "clip", media, &length) != ESP_OK || length != media_size ||
                !media_validate(media, length, &info))
                return ESP_ERR_INVALID_CRC;
        }
        reset_frame();
    }
    gpio_config_t pins = {.pin_bit_mask = (1ULL << 0) | (1ULL << 1), .mode = GPIO_MODE_OUTPUT};
    if ((err = gpio_config(&pins)) != ESP_OK)
        return err;
    spi_bus_config_t bus = {.mosi_io_num = 4,
                            .miso_io_num = -1,
                            .sclk_io_num = 2,
                            .quadwp_io_num = -1,
                            .quadhd_io_num = -1,
                            .max_transfer_sz = 1280};
    if ((err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO)) != ESP_OK)
        return err;
    spi_device_interface_config_t device = {
        .clock_speed_hz = 8000000, .mode = 0, .spics_io_num = 5, .queue_size = 1};
    if ((err = spi_bus_add_device(SPI2_HOST, &device, &spi)) != ESP_OK)
        return err;
    strip = spi_bus_dma_memory_alloc(SPI2_HOST, 1280, 0);
    if (!strip)
        return ESP_ERR_NO_MEM;
    if ((err = panel_init()) != ESP_OK)
        return err;
    heartbeat_ms = (uint32_t)(esp_timer_get_time() / 1000);
    driver_ready = true;
    /* One bounded frame, followed by a guaranteed 10 ms sleep, gives IDLE
       regular execution without doubling render time through time slicing. */
    if (xTaskCreate(display_task, "display", 4096, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        driver_ready = false;
        return ESP_ERR_NO_MEM;
    }
    diag_event("ST7735R SPI ready; visible panel still requires visual verification");
    return ESP_OK;
}
/* Lock order: operation -> display -> NVS internal. Renderer never takes operation. */
static bool begin_change(void)
{
    xSemaphoreTake(operation_mutex, portMAX_DELAY);
    if (!mutex || !storage_ready || !settings_healthy || ota_busy()) {
        xSemaphoreGive(operation_mutex);
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    return true;
}
static esp_err_t end_change(esp_err_t err)
{
    if (err != ESP_OK)
        strlcpy(last_error, "Display operation failed. Check saved content and diagnostics.",
                sizeof(last_error));
    else
        last_error[0] = 0;
    xSemaphoreGive(mutex);
    xSemaphoreGive(operation_mutex);
    return err;
}
esp_err_t display_install(const uint8_t *data, size_t size)
{
    media_info_t candidate;
    if (!media_validate(data, size, &candidate))
        return ESP_ERR_INVALID_ARG;
    uint8_t hash[32];
    char candidate_sha[65];
    sha256_bytes(data, size, hash);
    hex_encode(hash, 32, candidate_sha);
    if (!begin_change())
        return ESP_ERR_INVALID_STATE;
    /* Lost acknowledgements can retry the same clip without extra flash wear. */
    if (size == media_size && !strcmp(candidate_sha, media_sha))
        return end_change(ESP_OK);
    esp_err_t err = nvs_set_blob(storage, "clip", data, size);
    if (err == ESP_OK)
        err = nvs_commit(storage);
    if (err == ESP_OK) {
        if (!replay_loaded)
            memcpy(media, data, size);
        stored_clip = true;
        media_size = size;
        info = candidate;
        memcpy(media_sha, candidate_sha, sizeof(media_sha));
        if (!replay_loaded)
            reset_frame();
        diag_event("Display clip saved and verified");
    }
    return end_change(err);
}
esp_err_t display_remove(void)
{
    if (!begin_change())
        return ESP_ERR_INVALID_STATE;
    esp_err_t err = nvs_erase_key(storage, "clip");
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_commit(storage);
    if (err == ESP_OK) {
        media_size = 0;
        stored_clip = false;
        memset(&info, 0, sizeof(info));
        media_sha[0] = 0;
        if (!replay_loaded)
            reset_frame();
    }
    return end_change(err);
}
esp_err_t display_configure(const cJSON *request)
{
    if (!cJSON_IsObject(request) || !cJSON_GetArraySize(request) || cJSON_GetArraySize(request) > 5)
        return ESP_ERR_INVALID_ARG;
    if (!begin_change())
        return ESP_ERR_INVALID_STATE;
    display_config_t next = config;
    int chapter = -1;
    const cJSON *item;
    cJSON_ArrayForEach(item, request)
    {
        if (!strcmp(item->string, "mode") && cJSON_IsString(item)) {
            unsigned i = 0;
            while (i < MODE_COUNT && strcmp(item->valuestring, modes[i]))
                i++;
            if (i == MODE_COUNT || ((i == MODE_PLAY || i == MODE_PAUSE) && !media_size))
                return end_change(ESP_ERR_INVALID_ARG);
            next.mode = i;
        } else if (!strcmp(item->string, "profile") && cJSON_IsString(item)) {
            unsigned i = 0;
            while (i < 3 && strcmp(item->valuestring, profiles[i]))
                i++;
            if (i == 3)
                return end_change(ESP_ERR_INVALID_ARG);
            next.profile = i;
        } else if (!strcmp(item->string, "rotation") && cJSON_IsNumber(item) &&
                   item->valuedouble >= 0 && item->valuedouble <= 3 &&
                   item->valuedouble == item->valueint) {
            next.rotation = item->valueint;
        } else if (!strcmp(item->string, "replay_chapter") && cJSON_IsNumber(item) &&
                   item->valuedouble >= 0 && item->valuedouble < 32 &&
                   item->valuedouble == item->valueint) {
            chapter = item->valueint;
        } else if (!strcmp(item->string, "inverted") && cJSON_IsBool(item)) {
            next.inverted = cJSON_IsTrue(item);
        } else {
            return end_change(ESP_ERR_INVALID_ARG);
        }
    }
    if (chapter >= 0 && (!replay_mode(next.mode) || chapter != 0))
        return end_change(ESP_ERR_INVALID_ARG);
    next.crc = record_crc(&next, offsetof(display_config_t, crc));
    if (chapter < 0 && !memcmp(&next, &config, sizeof(config)))
        return end_change(ESP_OK);
    uint8_t *saved_clip = NULL;
    if (!replay_mode(next.mode) && replay_loaded && media_size) {
        saved_clip = malloc(media_size);
        size_t length = media_size;
        media_info_t checked;
        if (!saved_clip)
            return end_change(ESP_ERR_NO_MEM);
        if (nvs_get_blob(storage, "clip", saved_clip, &length) != ESP_OK || length != media_size ||
            !media_validate(saved_clip, length, &checked)) {
            free(saved_clip);
            return end_change(ESP_ERR_INVALID_CRC);
        }
    }
    esp_err_t err = ESP_OK;
    if (memcmp(&next, &config, sizeof(next))) {
        err = nvs_set_blob(storage, "config_v2", &next, sizeof(next));
        if (err == ESP_OK)
            err = nvs_commit(storage);
    }
    if (err == ESP_OK) {
        panel_dirty |= next.profile != config.profile || next.rotation != config.rotation ||
                       next.inverted != config.inverted;
        bool use_replay = replay_mode(next.mode);
        if (use_replay && !replay_loaded && !open_replay()) {
            driver_ready = false;
            free(saved_clip);
            return end_change(ESP_ERR_INVALID_CRC);
        }
        if (!use_replay && replay_loaded) {
            replay_loaded = false;
            if (saved_clip)
                memcpy(media, saved_clip, media_size);
            reset_frame();
        }
        if (chapter >= 0 && !builtin_loop_seek(&movie, 0)) {
            driver_ready = false;
            free(saved_clip);
            return end_change(ESP_ERR_INVALID_CRC);
        }
        config = next;
        dirty = true;
    }
    free(saved_clip);
    return end_change(err);
}
uint8_t *display_copy(size_t *size)
{
    *size = 0;
    if (!mutex)
        return NULL;
    xSemaphoreTake(mutex, portMAX_DELAY);
    uint8_t *result = media_size ? malloc(media_size) : NULL;
    if (result) {
        size_t length = media_size;
        if (replay_loaded) {
            if (nvs_get_blob(storage, "clip", result, &length) != ESP_OK || length != media_size) {
                free(result);
                result = NULL;
            }
        } else {
            memcpy(result, media, media_size);
        }
        if (result)
            *size = media_size;
    }
    xSemaphoreGive(mutex);
    return result;
}
cJSON *display_status(void)
{
    cJSON *j = cJSON_CreateObject();
    if (!j || !mutex)
        return j;
    bool suspended = ota_busy() && !ota_trial();
    xSemaphoreTake(mutex, portMAX_DELAY);
    cJSON_AddBoolToObject(j, "ready", display_healthy());
    cJSON_AddBoolToObject(j, "storage_ready", storage_ready);
    cJSON_AddBoolToObject(j, "has_stored_clip", stored_clip);
    cJSON_AddBoolToObject(j, "suspended", suspended);
    cJSON_AddStringToObject(j, "mode", modes[config.mode]);
    cJSON_AddStringToObject(j, "profile", profiles[config.profile]);
    cJSON_AddNumberToObject(j, "rotation", config.rotation);
    cJSON_AddBoolToObject(j, "inverted", config.inverted);
    cJSON_AddNumberToObject(j, "width", config.rotation & 1 ? 160 : 128);
    cJSON_AddNumberToObject(j, "height", config.rotation & 1 ? 128 : 160);
    cJSON_AddNumberToObject(j, "capacity", MEDIA_MAX_BYTES);
    cJSON_AddNumberToObject(j, "size", media_size);
    cJSON_AddStringToObject(j, "sha256", media_sha);
    cJSON_AddStringToObject(j, "title", info.title);
    cJSON_AddNumberToObject(j, "clip_width", info.width);
    cJSON_AddNumberToObject(j, "clip_height", info.height);
    cJSON_AddNumberToObject(j, "fps", info.fps);
    cJSON_AddNumberToObject(j, "frames", info.frames);
    cJSON_AddNumberToObject(j, "frame", frame);
    cJSON_AddNumberToObject(j, "rendered", rendered);
    cJSON_AddNumberToObject(j, "loops", loops);
    cJSON_AddNumberToObject(j, "late_frames", late_frames);
    cJSON_AddNumberToObject(j, "last_frame_us", last_frame_us);
    cJSON_AddNumberToObject(j, "max_frame_us", max_frame_us);
    cJSON_AddStringToObject(j, "error", last_error);
    cJSON *replay = cJSON_AddObjectToObject(j, "replay");
    cJSON_AddBoolToObject(replay, "active", replay_loaded && replay_mode(config.mode));
    cJSON_AddNumberToObject(replay, "bytes", replay_asset_end - replay_asset_start);
    cJSON_AddNumberToObject(replay, "frame", replay_loaded ? movie.frame : 0);
    cJSON_AddStringToObject(replay, "format", "ECL1");
    cJSON_AddStringToObject(replay, "title", "Neko wave");
    cJSON_AddNumberToObject(replay, "width", replay_asset_start[12]);
    cJSON_AddNumberToObject(replay, "height", replay_asset_start[13]);
    cJSON_AddNumberToObject(replay, "frames", replay_asset_start[16] | (uint16_t)replay_asset_start[17] << 8);
    cJSON_AddNumberToObject(replay, "period_us", 1000000 / replay_asset_start[14]);
    /* Keep the v1 restart selector compatible with older clients. */
    cJSON_AddNumberToObject(replay, "chapters", 1);
    cJSON_AddNumberToObject(replay, "loops", replay_loaded ? movie.loops : 0);
    cJSON_AddStringToObject(replay, "caption", caption_text(0));
    cJSON_AddNumberToObject(replay, "caption_index", 0);
    cJSON_AddBoolToObject(j, "brightness_control", false);
    cJSON_AddBoolToObject(j, "panel_feedback", false);
    xSemaphoreGive(mutex);
    return j;
}

void display_telemetry(void)
{
    if (!mutex)
        return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    ESP_LOGI("display",
             "DISPLAY "
             "{\"mode\":\"%s\",\"frame\":%lu,\"rendered\":%lu,\"loops\":%lu,\"last_frame_us\":%lu,"
             "\"late_frames\":%lu,\"caption\":%u,\"ready\":%s}",
             modes[config.mode], (unsigned long)(replay_loaded ? movie.frame : frame),
             (unsigned long)rendered, (unsigned long)(replay_loaded ? movie.loops : loops),
             (unsigned long)last_frame_us, (unsigned long)late_frames, 0u,
             display_healthy() ? "true" : "false");
    xSemaphoreGive(mutex);
}
