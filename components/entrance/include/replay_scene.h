#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REPLAY_SCENE_BYTES 2432
#define REPLAY_CHR_BYTES 8192
#define REPLAY_MAX_SPANS 8

/* NROM scene snapshots: two name tables, OAM, palette and bounded scroll spans.
   This is a renderer for recorded pictures, not a CPU or game emulator. */
bool replay_scene_valid(const uint8_t *scene);
void replay_scene_row(const uint8_t *scene, const uint8_t *tiles, unsigned y, uint8_t out[256]);
void replay_scene_frame(const uint8_t *scene, const uint8_t *tiles, uint8_t out[256 * 240]);
