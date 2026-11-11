#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Transport-independent, allocation-free simulation. All clocks are device-owned. */
#define ARCADE_WIDTH 160
#define ARCADE_HEIGHT 128
#define ARCADE_FRAME_BYTES (ARCADE_WIDTH * ARCADE_HEIGHT / 2)
#define ARCADE_TURN_MS 300000u
#define ARCADE_IDLE_MS 45000u
#define ARCADE_MENU_IDLE_MS 25000u
#define ARCADE_LINK_MS 10000u
#define ARCADE_COOLDOWN_MS 10000u
enum arcade_stage { ARCADE_IDLE, ARCADE_MENU, ARCADE_COUNTDOWN, ARCADE_PLAY, ARCADE_RESULT };
enum arcade_game { ARCADE_TETRIS, ARCADE_PONG, ARCADE_PACMAN, ARCADE_GAME_COUNT };
enum arcade_button {
    ARCADE_LEFT = 1,
    ARCADE_RIGHT = 2,
    ARCADE_UP = 4,
    ARCADE_DOWN = 8,
    ARCADE_ROTATE = 16,
    ARCADE_DROP = 32,
    ARCADE_HOLD = 64
};
enum arcade_action {
    ARCADE_INPUT,
    ARCADE_SELECT,
    ARCADE_READY,
    ARCADE_MENU_ACTION,
    ARCADE_LEAVE,
    ARCADE_HEARTBEAT
};
enum arcade_result {
    ARCADE_OK,
    ARCADE_BUSY,
    ARCADE_INVALID,
    ARCADE_STALE,
    ARCADE_NOT_PLAYER,
    ARCADE_RATE_LIMITED
};
typedef struct {
    uint64_t id, last_seen, last_action, last_input, repeat_at;
    uint32_t sequence;
    uint16_t held;
    int16_t axis;
    bool ready, sequence_set;
} arcade_player_t;
typedef struct {
    uint8_t cells[22][10], bag[7], bag_at, kind, rotation, next, held_kind;
    int8_t x, y;
    uint8_t lock_resets;
    bool hold_used;
    uint32_t lines, score, clear_mask;
    uint64_t gravity_at, lock_at, clear_at;
} arcade_tetris_t;
typedef struct {
    float x, y, vx, vy, paddle[2], target, trail_x[5], trail_y[5];
    uint8_t points[2], rally;
    uint64_t serve_at, react_at;
    bool ai;
} arcade_pong_t;
#define ARCADE_MAZE_W 25
#define ARCADE_MAZE_H 19
typedef struct {
    int8_t x, y, direction, wanted;
} arcade_actor_t;
typedef struct {
    uint8_t pellets[ARCADE_MAZE_H][ARCADE_MAZE_W];
    arcade_actor_t player, ghosts[4];
    uint16_t remaining, score, eaten;
    uint8_t lives, level;
    uint64_t move_at, ghost_at, power_until, respawn_at;
} arcade_pacman_t;
typedef struct {
    arcade_player_t players[2];
    enum arcade_stage stage;
    enum arcade_game game;
    uint64_t now, turn_until, phase_until, cooldown_until, simulation_at;
    uint32_t rng, revision, rounds;
    uint8_t winner; /* 0/1 player, 2 house, 3 score-only */
    char reason[40];
    union {
        arcade_tetris_t tetris;
        arcade_pong_t pong;
        arcade_pacman_t pacman;
    } game_state;
} arcade_t;

void arcade_init(arcade_t *a, uint32_t seed, uint64_t now);
int arcade_player(const arcade_t *a, uint64_t id);
enum arcade_result arcade_join(arcade_t *a, uint64_t id, uint64_t now);
enum arcade_result arcade_command(arcade_t *a, uint64_t id, uint32_t sequence,
                                  enum arcade_action action, int value, int axis, uint64_t now);
void arcade_tick(arcade_t *a, uint64_t now);
void arcade_release(arcade_t *a, uint64_t now, const char *reason);
void arcade_render(const arcade_t *a, uint8_t pixels[ARCADE_FRAME_BYTES]);
uint16_t arcade_color(uint8_t index);
bool arcade_maze_wall(int x, int y);
const char *arcade_game_name(enum arcade_game game);
