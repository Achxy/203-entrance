#include "arcade_core.h"
#include "font8x8.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t random32(arcade_t *a)
{
    uint32_t x = a->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return a->rng = x;
}
static int clampi(int n, int lo, int hi)
{
    return n < lo ? lo : n > hi ? hi : n;
}
static float clampf(float n, float lo, float hi)
{
    return n < lo ? lo : n > hi ? hi : n;
}
static float approach(float n, float target, float amount)
{
    return n + clampf(target - n, -amount, amount);
}
static void reason(arcade_t *a, const char *s)
{
    snprintf(a->reason, sizeof(a->reason), "%s", s);
    a->revision++;
}
const char *arcade_game_name(enum arcade_game g)
{
    static const char *names[] = {"TETRIS", "PONG", "PAC-MAN"};
    return g < ARCADE_GAME_COUNT ? names[g] : "ARCADE";
}
int arcade_player(const arcade_t *a, uint64_t id)
{
    for (int i = 0; i < 2; i++)
        if (id && a->players[i].id == id)
            return i;
    return -1;
}
void arcade_init(arcade_t *a, uint32_t seed, uint64_t now)
{
    memset(a, 0, sizeof(*a));
    a->rng = seed ? seed : 1;
    a->now = now;
    a->winner = 3;
}
void arcade_release(arcade_t *a, uint64_t now, const char *why)
{
    memset(a->players, 0, sizeof(a->players));
    a->stage = ARCADE_IDLE;
    a->turn_until = 0;
    a->cooldown_until = now + ARCADE_COOLDOWN_MS;
    reason(a, why);
}
static void menu(arcade_t *a, uint64_t now, const char *why)
{
    a->stage = ARCADE_MENU;
    a->phase_until = 0;
    for (int i = 0; i < 2; i++) {
        a->players[i].ready = false;
        a->players[i].held = 0;
        a->players[i].last_action = now;
    }
    reason(a, why);
}
static void leave(arcade_t *a, int slot, uint64_t now, const char *why)
{
    memset(&a->players[slot], 0, sizeof(a->players[slot]));
    if (slot == 0) {
        a->players[0] = a->players[1];
        memset(&a->players[1], 0, sizeof(a->players[1]));
    }
    if (!a->players[0].id)
        arcade_release(a, now, why);
    else
        menu(a, now, why);
}
enum arcade_result arcade_join(arcade_t *a, uint64_t id, uint64_t now)
{
    arcade_tick(a, now);
    if (!id)
        return ARCADE_INVALID;
    if (arcade_player(a, id) >= 0)
        return ARCADE_OK;
    if (now < a->cooldown_until || (a->stage != ARCADE_IDLE && a->stage != ARCADE_MENU) ||
        a->players[1].id)
        return ARCADE_BUSY;
    int slot = a->players[0].id ? 1 : 0;
    a->players[slot] =
        (arcade_player_t){.id = id, .last_seen = now, .last_action = now, .last_input = now};
    if (!slot) {
        a->turn_until = now + ARCADE_TURN_MS;
        a->game = ARCADE_TETRIS;
    } else
        a->game = ARCADE_PONG;
    menu(a, now, slot ? "Player two joined" : "Choose a game");
    return ARCADE_OK;
}

/* Seven tetrominoes, four orientations, cells stored in a 4x4 mask. */
static const uint16_t shapes[7][4] = {
    {0x00f0, 0x4444, 0x0f00, 0x2222}, {0x0660, 0x0660, 0x0660, 0x0660},
    {0x0072, 0x0262, 0x0270, 0x0232}, {0x0036, 0x0462, 0x0360, 0x0231},
    {0x0063, 0x0264, 0x0630, 0x0132}, {0x0071, 0x0226, 0x0470, 0x0322},
    {0x0074, 0x0622, 0x0170, 0x0223}};
static bool occupied(uint8_t k, uint8_t r, int x, int y)
{
    return (shapes[k][r & 3] >> (y * 4 + x)) & 1u;
}
static bool fits(const arcade_tetris_t *t, int x, int y, int r)
{
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            if (occupied(t->kind, r, i, j)) {
                int px = x + i, py = y + j;
                if (px < 0 || px >= 10 || py < 0 || py >= 22 || t->cells[py][px])
                    return false;
            }
    return true;
}
static uint8_t bag(arcade_t *a)
{
    arcade_tetris_t *t = &a->game_state.tetris;
    if (t->bag_at >= 7) {
        for (int i = 0; i < 7; i++)
            t->bag[i] = i;
        for (int i = 6; i > 0; i--) {
            int j = random32(a) % (i + 1);
            uint8_t n = t->bag[i];
            t->bag[i] = t->bag[j];
            t->bag[j] = n;
        }
        t->bag_at = 0;
    }
    return t->bag[t->bag_at++];
}
static void result(arcade_t *a, uint8_t winner, const char *why)
{
    a->stage = ARCADE_RESULT;
    a->winner = winner;
    a->phase_until = a->now + 5000;
    a->rounds++;
    for (int i = 0; i < 2; i++)
        a->players[i].held = 0;
    reason(a, why);
}
static void spawn(arcade_t *a)
{
    arcade_tetris_t *t = &a->game_state.tetris;
    t->kind = t->next;
    t->next = bag(a);
    t->x = 3;
    t->y = 0;
    t->rotation = 0;
    t->lock_at = 0;
    t->lock_resets = 0;
    t->hold_used = false;
    t->gravity_at = a->now;
    if (!fits(t, t->x, t->y, t->rotation))
        result(a, 3, "STACK COMPLETE");
}
static void lock_piece(arcade_t *a)
{
    arcade_tetris_t *t = &a->game_state.tetris;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++)
            if (occupied(t->kind, t->rotation, i, j))
                t->cells[t->y + j][t->x + i] = t->kind + 1;
    t->clear_mask = 0;
    for (int y = 0; y < 22; y++) {
        bool full = true;
        for (int x = 0; x < 10; x++)
            if (!t->cells[y][x])
                full = false;
        if (full)
            t->clear_mask |= 1u << y;
    }
    if (t->clear_mask)
        t->clear_at = a->now + 180;
    else
        spawn(a);
}
static void tetris_move(arcade_t *a, unsigned edge, int horizontal)
{
    arcade_tetris_t *t = &a->game_state.tetris;
    if (t->clear_mask)
        return;
    bool moved = false;
    if (horizontal && fits(t, t->x + horizontal, t->y, t->rotation)) {
        t->x += horizontal;
        moved = true;
    }
    if (edge & ARCADE_ROTATE) {
        /* Bounded wall/floor kicks, including two-cell I-piece recovery. */
        static const int8_t kicks[][2] = {{0, 0},  {-1, 0},  {1, 0},  {-2, 0}, {2, 0},
                                          {0, -1}, {-1, -1}, {1, -1}, {0, -2}};
        for (unsigned i = 0; i < sizeof(kicks) / sizeof(kicks[0]); i++)
            if (fits(t, t->x + kicks[i][0], t->y + kicks[i][1], t->rotation + 1)) {
                t->x += kicks[i][0];
                t->y += kicks[i][1];
                t->rotation = (t->rotation + 1) & 3;
                moved = true;
                break;
            }
    }
    if (edge & ARCADE_HOLD) {
        if (!t->hold_used) {
            uint8_t old = t->kind;
            if (t->held_kind == 255)
                spawn(a);
            else {
                t->kind = t->held_kind;
                t->x = 3;
                t->y = 0;
                t->rotation = 0;
                t->lock_at = 0;
                t->lock_resets = 0;
                if (!fits(t, t->x, t->y, 0))
                    result(a, 3, "STACK COMPLETE");
            }
            t->held_kind = old;
            t->hold_used = true;
        }
        return;
    }
    if (edge & ARCADE_DROP) {
        while (fits(t, t->x, t->y + 1, t->rotation)) {
            t->y++;
            t->score += 2;
        }
        lock_piece(a);
        return;
    }
    if (moved && t->lock_at && t->lock_resets < 15) {
        t->lock_at = a->now;
        t->lock_resets++;
    }
}
static void tetris_tick(arcade_t *a)
{
    arcade_tetris_t *t = &a->game_state.tetris;
    if (t->clear_mask) {
        if (a->now >= t->clear_at) {
            int dst = 21, n = 0;
            for (int y = 21; y >= 0; y--) {
                if (t->clear_mask & (1u << y))
                    n++;
                else
                    memmove(t->cells[dst--], t->cells[y], 10);
            }
            while (dst >= 0)
                memset(t->cells[dst--], 0, 10);
            static const int scores[] = {0, 100, 300, 500, 800};
            t->score += scores[n > 4 ? 4 : n] * (1 + t->lines / 10);
            t->lines += n;
            t->clear_mask = 0;
            spawn(a);
        }
        return;
    }
    arcade_player_t *p = &a->players[0];
    int horizontal = (!!(p->held & ARCADE_RIGHT)) - (!!(p->held & ARCADE_LEFT));
    if (horizontal && a->now >= p->repeat_at) {
        tetris_move(a, 0, horizontal);
        p->repeat_at = a->now + 90;
    }
    unsigned interval = 700u / (1 + t->lines / 10);
    if (interval < 70)
        interval = 70;
    if (p->held & ARCADE_DOWN)
        interval = 40;
    if (a->now - t->gravity_at >= interval) {
        t->gravity_at = a->now;
        if (fits(t, t->x, t->y + 1, t->rotation)) {
            t->y++;
            t->lock_at = 0;
            if (p->held & ARCADE_DOWN)
                t->score++;
        }
    }
    if (!fits(t, t->x, t->y + 1, t->rotation)) {
        if (!t->lock_at)
            t->lock_at = a->now;
        if (a->now - t->lock_at >= 500)
            lock_piece(a);
    } else
        t->lock_at = 0;
}

/* Entire connected compact level, 25x19 cells at 5x5 pixels. */
static const char maze[ARCADE_MAZE_H][ARCADE_MAZE_W + 1] = {
    "#########################", "#o.........#...........o#", "#.###.####.#.####.###.#.#",
    "#.......................#", "#.###.#.#######.#.###.#.#", "#.....#....#....#.....#.#",
    "###.#.####.#.####.#.###.#", "#...#.............#.....#", "#.#####.###.###.#####.#.#",
    "........#     #..........", "#.#####.# ### #.#####.#.#", "#.......#     #.........#",
    "#.###.###.###.###.###.#.#", "#...#.....#.#.....#...#.#", "###.#.###.#.#.###.#.###.#",
    "#.......................#", "#.###.####.#.####.###.#.#", "#o.....................o#",
    "#########################"};
bool arcade_maze_wall(int x, int y)
{
    return x < 0 || x >= ARCADE_MAZE_W || y < 0 || y >= ARCADE_MAZE_H || maze[y][x] == '#';
}
static const int8_t dx[] = {-1, 1, 0, 0}, dy[] = {0, 0, -1, 1};
static bool step(arcade_actor_t *p, int dir)
{
    int x = p->x + dx[dir], y = p->y + dy[dir];
    if (y == 9) {
        if (x < 0)
            x = 24;
        if (x > 24)
            x = 0;
    }
    if (arcade_maze_wall(x, y))
        return false;
    p->x = x;
    p->y = y;
    p->direction = dir;
    return true;
}
static void pac_positions(arcade_pacman_t *p)
{
    p->player = (arcade_actor_t){.x = 12, .y = 15, .direction = 0, .wanted = 0};
    for (int i = 0; i < 4; i++)
        p->ghosts[i] = (arcade_actor_t){.x = 10 + i, .y = 9, .direction = 2, .wanted = 2};
}
static void pac_level(arcade_t *a)
{
    arcade_pacman_t *p = &a->game_state.pacman;
    p->remaining = 0;
    for (int y = 0; y < 19; y++)
        for (int x = 0; x < 25; x++) {
            p->pellets[y][x] = maze[y][x] == '.' ? 1 : maze[y][x] == 'o' ? 2 : 0;
            p->remaining += p->pellets[y][x] != 0;
        }
    pac_positions(p);
    p->power_until = 0;
    p->respawn_at = a->now + 1200;
    p->move_at = a->now;
    p->ghost_at = a->now;
}
static void pac_collision(arcade_t *a)
{
    arcade_pacman_t *p = &a->game_state.pacman;
    for (int i = 0; i < 4; i++)
        if (p->player.x == p->ghosts[i].x && p->player.y == p->ghosts[i].y) {
            if (a->now < p->power_until) {
                p->score += (uint16_t)(200u << (p->eaten < 3 ? p->eaten++ : 3));
                p->ghosts[i] = (arcade_actor_t){.x = 12, .y = 9, .direction = 2};
            } else {
                if (--p->lives == 0) {
                    result(a, 3, "NICE RUN");
                    return;
                }
                pac_positions(p);
                p->power_until = 0;
                p->respawn_at = a->now + 1500;
                return;
            }
        }
}
static void pac_tick(arcade_t *a)
{
    arcade_pacman_t *p = &a->game_state.pacman;
    if (a->now < p->respawn_at)
        return;
    unsigned pace = clampi(150 - (int)p->level * 5, 95, 150);
    if (a->now - p->move_at >= pace) {
        p->move_at = a->now;
        if (!step(&p->player, p->player.wanted))
            step(&p->player, p->player.direction);
        uint8_t *pellet = &p->pellets[p->player.y][p->player.x];
        if (*pellet) {
            p->score += *pellet == 2 ? 50 : 10;
            if (*pellet == 2) {
                p->power_until = a->now + 6000;
                p->eaten = 0;
            }
            *pellet = 0;
            if (!--p->remaining) {
                p->level++;
                pac_level(a);
                return;
            }
        }
        pac_collision(a);
    }
    if (a->stage != ARCADE_PLAY || a->now < p->respawn_at)
        return;
    if (a->now - p->ghost_at >= pace + 35 + (a->now < p->power_until ? 65 : 0)) {
        p->ghost_at = a->now;
        for (int i = 0; i < 4; i++) {
            arcade_actor_t *g = &p->ghosts[i];
            int best = -1, best_distance = 100000;
            bool scatter = (a->now / 7000) % 4 == 0;
            int tx = scatter ? (i & 1 ? 23 : 1) : p->player.x,
                ty = scatter ? (i & 2 ? 17 : 1) : p->player.y;
            if (!scatter && i == 1) {
                tx += dx[p->player.direction] * 3;
                ty += dy[p->player.direction] * 3;
            }
            if (!scatter && i == 2) {
                tx = 24 - tx;
            }
            if (!scatter && i == 3 && abs(g->x - tx) + abs(g->y - ty) < 6) {
                tx = 1;
                ty = 17;
            }
            for (int d = 0; d < 4; d++) {
                arcade_actor_t q = *g;
                if (d == (g->direction ^ 1) || !step(&q, d))
                    continue;
                int distance = (q.x - tx) * (q.x - tx) + (q.y - ty) * (q.y - ty);
                if (a->now < p->power_until)
                    distance = -distance;
                if (distance < best_distance) {
                    best = d;
                    best_distance = distance;
                }
            }
            if (best < 0)
                best = g->direction ^ 1;
            step(g, best);
            pac_collision(a);
            if (a->stage != ARCADE_PLAY || a->now < p->respawn_at)
                break;
        }
    }
}

static float reflection(float y)
{
    const float span = 96;
    float r = fmodf(y - 22, 2 * span);
    if (r < 0)
        r += 2 * span;
    return 22 + (r > span ? 2 * span - r : r);
}
static void serve(arcade_t *a)
{
    arcade_pong_t *p = &a->game_state.pong;
    p->x = 80;
    p->y = 70;
    p->vx = (p->points[0] + p->points[1]) & 1 ? -66 : 66;
    p->vy = (int)(random32(a) % 41) - 20;
    p->serve_at = a->now + 900;
    p->react_at = a->now + 180;
    p->target = reflection(p->y + p->vy * (146 - p->x) / fabsf(p->vx));
    p->rally = 0;
    for (int i = 0; i < 5; i++) {
        p->trail_x[i] = 80;
        p->trail_y[i] = 70;
    }
}
static void pong_tick(arcade_t *a)
{
    arcade_pong_t *p = &a->game_state.pong;
    const float dt = .02f;
    for (int i = 0; i < 2; i++) {
        if (i == 1 && p->ai) {
            if (a->now >= p->react_at)
                p->paddle[i] = approach(p->paddle[i], p->vx > 0 ? p->target : 70, 110 * dt);
        } else {
            arcade_player_t *u = &a->players[i];
            float direction = u->axis / 1000.f;
            if (u->held & (ARCADE_UP | ARCADE_DOWN))
                direction = (!!(u->held & ARCADE_DOWN)) - (!!(u->held & ARCADE_UP));
            p->paddle[i] = clampf(p->paddle[i] + direction * (p->ai ? 60 : 105) * dt, 31, 109);
        }
    }
    if (a->now < p->serve_at)
        return;
    for (int i = 4; i > 0; i--) {
        p->trail_x[i] = p->trail_x[i - 1];
        p->trail_y[i] = p->trail_y[i - 1];
    }
    p->trail_x[0] = p->x;
    p->trail_y[0] = p->y;
    float oldx = p->x, oldy = p->y, oldvy = p->vy;
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    if (p->y < 22) {
        p->y = 44 - p->y;
        p->vy = fabsf(p->vy);
    }
    if (p->y > 118) {
        p->y = 236 - p->y;
        p->vy = -fabsf(p->vy);
    }
    int side = p->vx < 0 ? 0 : 1;
    float plane = side ? 146 : 14;
    if ((side == 0 && oldx >= plane && p->x <= plane) ||
        (side == 1 && oldx <= plane && p->x >= plane)) {
        float hit = reflection(oldy + oldvy * ((plane - oldx) / p->vx));
        if (fabsf(hit - p->paddle[side]) <= 13) {
            p->x = side ? 145.9f : 14.1f;
            p->y = hit;
            p->rally++;
            float speed = fminf(110, 66 + p->rally * 4);
            p->vx = side ? -speed : speed;
            p->vy = clampf((hit - p->paddle[side]) * 8, -90, 90);
            if (p->ai) {
                if (side == 0) {
                    p->vx = 80;
                    p->target = reflection(p->y + p->vy * (146 - p->x) / p->vx);
                    p->react_at = a->now + 180;
                } else if (p->rally >= 8) {
                    /* Explicit house-mode finishing return. At most .413 s to
                       travel, 60 px/s human speed, >=48 px target displacement.
                       24.8 movement + 13 hit radius < 48: no legal interception.
                       AI incoming speed remains bounded to retain smooth motion. */
                    float target = p->paddle[0] < 70 ? 118 : 22;
                    p->vx = -320;
                    p->vy = (target - p->y) / (132.f / 320.f);
                }
            }
        }
    }
    if (p->x < 2 || p->x > 158) {
        unsigned scorer = p->x < 2 ? 1 : 0;
        if (++p->points[scorer] >= 5) {
            result(a, p->ai && scorer == 1 ? 2 : scorer,
                   p->ai && scorer == 1 ? "HOUSE WINS" : "MATCH COMPLETE");
            return;
        }
        serve(a);
    }
}
static void start_game(arcade_t *a)
{
    memset(&a->game_state, 0, sizeof(a->game_state));
    a->stage = ARCADE_COUNTDOWN;
    a->phase_until = a->now + 2200;
    a->winner = 3;
    if (a->game == ARCADE_TETRIS) {
        arcade_tetris_t *t = &a->game_state.tetris;
        t->bag_at = 7;
        t->held_kind = 255;
        t->next = bag(a);
        spawn(a);
    } else if (a->game == ARCADE_PONG) {
        arcade_pong_t *p = &a->game_state.pong;
        p->ai = !a->players[1].id;
        p->paddle[0] = p->paddle[1] = 70;
        serve(a);
    } else {
        a->game_state.pacman.lives = 3;
        a->game_state.pacman.level = 1;
        pac_level(a);
    }
    reason(a, "GET READY");
}
enum arcade_result arcade_command(arcade_t *a, uint64_t id, uint32_t seq, enum arcade_action action,
                                  int value, int axis, uint64_t now)
{
    arcade_tick(a, now);
    int slot = arcade_player(a, id);
    if (slot < 0)
        return ARCADE_NOT_PLAYER;
    arcade_player_t *p = &a->players[slot];
    if (action < ARCADE_INPUT || action > ARCADE_HEARTBEAT || value < 0 || axis < -1000 ||
        axis > 1000)
        return ARCADE_INVALID;
    if (p->sequence_set && seq <= p->sequence)
        return ARCADE_STALE;
    if (action == ARCADE_INPUT && ((unsigned)value > 127))
        return ARCADE_INVALID;
    if (action == ARCADE_SELECT && (slot || a->stage != ARCADE_MENU || value >= ARCADE_GAME_COUNT ||
                                    (a->players[1].id && value != ARCADE_PONG)))
        return ARCADE_INVALID;
    if (action == ARCADE_READY && (a->stage != ARCADE_MENU || value > 1))
        return ARCADE_INVALID;
    if (action == ARCADE_MENU_ACTION && a->stage == ARCADE_MENU)
        return ARCADE_INVALID;
    if (action == ARCADE_INPUT && now - p->last_input < 20)
        return ARCADE_RATE_LIMITED;
    p->sequence = seq;
    p->sequence_set = true;
    p->last_seen = now;
    if (action == ARCADE_LEAVE) {
        leave(a, slot, now, "Player left");
        return ARCADE_OK;
    }
    if (action == ARCADE_HEARTBEAT)
        return ARCADE_OK;
    if (action == ARCADE_INPUT) {
        unsigned edge = (unsigned)value & ~p->held;
        if (p->held != value || abs(p->axis - axis) >= 50)
            p->last_action = now;
        p->held = value;
        p->axis = axis;
        p->last_input = now;
        if (a->stage == ARCADE_PLAY && a->game == ARCADE_TETRIS && !slot) {
            int horizontal = (!!(edge & ARCADE_RIGHT)) - (!!(edge & ARCADE_LEFT));
            tetris_move(a, edge, horizontal);
            if (horizontal)
                p->repeat_at = now + 180;
        }
        if (a->stage == ARCADE_PLAY && a->game == ARCADE_PACMAN && !slot) {
            int d = -1;
            if (edge & ARCADE_LEFT)
                d = 0;
            if (edge & ARCADE_RIGHT)
                d = 1;
            if (edge & ARCADE_UP)
                d = 2;
            if (edge & ARCADE_DOWN)
                d = 3;
            if (d >= 0)
                a->game_state.pacman.player.wanted = d;
        }
        return ARCADE_OK;
    }
    p->last_action = now;
    if (action == ARCADE_MENU_ACTION) {
        menu(a, now, "Back to menu");
        return ARCADE_OK;
    }
    if (action == ARCADE_SELECT) {
        a->game = value;
        for (int i = 0; i < 2; i++)
            a->players[i].ready = false;
        reason(a, "Choose a game");
    }
    if (action == ARCADE_READY) {
        p->ready = value;
        a->revision++;
        if (a->players[0].ready && (!a->players[1].id || a->players[1].ready))
            start_game(a);
    }
    return ARCADE_OK;
}
void arcade_tick(arcade_t *a, uint64_t now)
{
    if (now < a->now)
        return;
    a->now = now;
    if (a->stage == ARCADE_IDLE)
        return;
    if (now >= a->turn_until) {
        arcade_release(a, now, "Turn complete - thanks for playing");
        return;
    }
    for (int i = 1; i >= 0; i--)
        if (a->players[i].id) {
            arcade_player_t *p = &a->players[i];
            if (now - p->last_seen >= ARCADE_LINK_MS) {
                leave(a, i, now, "Controller disconnected");
                continue;
            }
            if (now - p->last_action >=
                (a->stage == ARCADE_MENU ? ARCADE_MENU_IDLE_MS : ARCADE_IDLE_MS)) {
                leave(a, i, now, "Idle player released");
                continue;
            }
            if (now - p->last_input > 300) {
                p->held = 0;
                p->axis = 0;
            }
        }
    if (a->stage == ARCADE_IDLE)
        return;
    if (a->stage == ARCADE_COUNTDOWN && now >= a->phase_until) {
        a->stage = ARCADE_PLAY;
        a->simulation_at = now;
        reason(a, "PLAY");
    }
    if (a->stage == ARCADE_RESULT && now >= a->phase_until) {
        menu(a, now, "Choose another game");
        return;
    }
    if (a->stage != ARCADE_PLAY)
        return;
    unsigned steps = 0;
    while (now - a->simulation_at >= 20 && steps++ < 5) {
        a->simulation_at += 20;
        if (a->game == ARCADE_PONG)
            pong_tick(a);
        else if (a->game == ARCADE_TETRIS)
            tetris_tick(a);
        else
            pac_tick(a);
        if (a->stage != ARCADE_PLAY)
            break;
    }
    if (now - a->simulation_at > 100)
        a->simulation_at = now;
}

static const uint16_t palette[16] = {0x0862, 0xff9a, 0xff00, 0xf9e7, 0x2e77, 0x64df,
                                     0xa3df, 0xfca7, 0x39c9, 0x18e5, 0xffff, 0x8c91,
                                     0x1be8, 0x62cd, 0x3186, 0xce15};
uint16_t arcade_color(uint8_t index)
{
    return palette[index & 15];
}
static void dot(uint8_t *f, int x, int y, int c)
{
    if (x < 0 || x >= 160 || y < 0 || y >= 128)
        return;
    unsigned n = y * 160 + x;
    f[n / 2] = (n & 1) ? (f[n / 2] & 0xf0) | (c & 15) : (f[n / 2] & 15) | ((c & 15) << 4);
}
static void rect(uint8_t *f, int x, int y, int w, int h, int c)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            dot(f, x + i, y + j, c);
}
static void text(uint8_t *f, int x, int y, const char *s, int c, int scale)
{
    for (; *s; s++, x += 8 * scale) {
        unsigned ch = (unsigned char)*s;
        if (ch > 127)
            ch = '?';
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++)
                if (font8x8_basic[ch][j] & (1 << i))
                    rect(f, x + i * scale, y + j * scale, scale, scale, c);
    }
}
static void number(uint8_t *f, int x, int y, unsigned n, int c, int scale)
{
    char s[16];
    snprintf(s, sizeof(s), "%u", n);
    text(f, x, y, s, c, scale);
}
static void block(uint8_t *f, int x, int y, int c)
{
    rect(f, x, y, 5, 5, c);
    rect(f, x, y, 4, 1, 1);
    dot(f, x + 4, y + 4, 9);
}
static const uint8_t piece_colors[7] = {4, 2, 6, 12, 3, 5, 7};
static void render_tetris(const arcade_t *a, uint8_t *f)
{
    const arcade_tetris_t *t = &a->game_state.tetris;
    int ox = 55, oy = 20;
    rect(f, ox - 2, oy - 2, 54, 104, 8);
    rect(f, ox, oy, 50, 100, 9);
    for (int y = 2; y < 22; y++)
        for (int x = 0; x < 10; x++)
            if (t->cells[y][x])
                block(f, ox + x * 5, oy + (y - 2) * 5,
                      (t->clear_mask & (1u << y)) && a->now % 120 < 60
                          ? 10
                          : piece_colors[t->cells[y][x] - 1]);
    if (!t->clear_mask) {
        int gy = t->y;
        while (fits(t, t->x, gy + 1, t->rotation))
            gy++;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                if (occupied(t->kind, t->rotation, x, y)) {
                    if (gy + y >= 2) {
                        int px = ox + (t->x + x) * 5, py = oy + (gy + y - 2) * 5;
                        rect(f, px, py, 5, 1, 8);
                        rect(f, px, py + 4, 5, 1, 8);
                        dot(f, px, py + 2, 8);
                        dot(f, px + 4, py + 2, 8);
                    }
                    if (t->y + y >= 2)
                        block(f, ox + (t->x + x) * 5, oy + (t->y + y - 2) * 5,
                              piece_colors[t->kind]);
                }
    }
    text(f, 4, 23, "SCORE", 11, 1);
    number(f, 4, 34, t->score, 1, 1);
    text(f, 4, 53, "LINES", 11, 1);
    number(f, 4, 64, t->lines, 2, 1);
    text(f, 4, 86, "LEVEL", 11, 1);
    number(f, 4, 97, 1 + t->lines / 10, 4, 1);
    text(f, 116, 23, "NEXT", 11, 1);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (occupied(t->next, 0, x, y))
                block(f, 116 + x * 5, 38 + y * 5, piece_colors[t->next]);
    text(f, 116, 73, "HOLD", 11, 1);
    if (t->held_kind < 7)
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                if (occupied(t->held_kind, 0, x, y))
                    block(f, 116 + x * 5, 88 + y * 5, piece_colors[t->held_kind]);
}
static void circle(uint8_t *f, int x, int y, int r, int c)
{
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r)
                dot(f, x + i, y + j, c);
}
static void render_pong(const arcade_t *a, uint8_t *f)
{
    const arcade_pong_t *p = &a->game_state.pong;
    for (int y = 20; y < 122; y += 8)
        rect(f, 79, y, 2, 3, 8);
    rect(f, 5, 17, 150, 1, 8);
    rect(f, 5, 122, 150, 1, 8);
    number(f, 48, 22, p->points[0], 4, 2);
    number(f, 101, 22, p->points[1], 3, 2);
    for (int i = 4; i >= 0; i--)
        circle(f, p->trail_x[i], p->trail_y[i], i > 2 ? 1 : 2, 9 + i);
    rect(f, 8, (int)p->paddle[0] - 11, 4, 22, 4);
    rect(f, 148, (int)p->paddle[1] - 11, 4, 22, 3);
    rect(f, 8, (int)p->paddle[0] - 11, 1, 20, 1);
    rect(f, 148, (int)p->paddle[1] - 11, 1, 20, 1);
    circle(f, p->x, p->y, 2, 1);
}
static void render_pacman(const arcade_t *a, uint8_t *f)
{
    const arcade_pacman_t *p = &a->game_state.pacman;
    int ox = 17, oy = 24;
    for (int y = 0; y < 19; y++)
        for (int x = 0; x < 25; x++) {
            int px = ox + x * 5, py = oy + y * 5;
            if (arcade_maze_wall(x, y)) {
                rect(f, px, py, 5, 5, 5);
                rect(f, px + 1, py + 1, 3, 3, 13);
            } else if (p->pellets[y][x] == 1)
                dot(f, px + 2, py + 2, 1);
            else if (p->pellets[y][x] == 2 && a->now % 500 < 350)
                circle(f, px + 2, py + 2, 2, 2);
        }
    int px = ox + p->player.x * 5 + 2, py = oy + p->player.y * 5 + 2;
    circle(f, px, py, 2, 2);
    if (a->now % 200 < 120) {
        dot(f, px + dx[p->player.direction], py + dy[p->player.direction], 0);
        dot(f, px + 2 * dx[p->player.direction], py + 2 * dy[p->player.direction], 0);
    }
    for (int i = 0; i < 4; i++) {
        int gx = ox + p->ghosts[i].x * 5, gy = oy + p->ghosts[i].y * 5;
        int color = a->now < p->power_until ? 5 : (int[]){3, 6, 4, 7}[i];
        rect(f, gx, gy + 1, 5, 4, color);
        rect(f, gx + 1, gy, 3, 1, color);
        dot(f, gx + 1, gy + 1, 10);
        dot(f, gx + 3, gy + 1, 10);
        dot(f, gx + (a->now / 160 % 2 ? 1 : 3), gy + 4, 0);
    }
    number(f, 114, 5, p->score, 1, 1);
    for (int i = 0; i < p->lives; i++)
        circle(f, 6, 90 + i * 10, 2, 2);
}
void arcade_render(const arcade_t *a, uint8_t f[ARCADE_FRAME_BYTES])
{
    memset(f, 0, ARCADE_FRAME_BYTES);
    text(f, 6, 5, "203", 2, 1);
    text(f, 34, 5, arcade_game_name(a->game), 1, 1);
    rect(f, 5, 15, 150, 1, 8);
    if (a->stage == ARCADE_MENU) {
        text(f, 6, 23, a->players[1].id ? "TWO PLAYERS" : "YOUR TURN", 11, 1);
        for (int g = 0; g < 3; g++) {
            int y = 39 + g * 23;
            bool selected = g == (int)a->game;
            rect(f, 5, y, 150, 20, selected ? 2 : 9);
            text(f, 12, y + 6, arcade_game_name(g), selected ? 0 : 1, 1);
            text(f, 101, y + 6, g == 1 ? (a->players[1].id ? "P1 / P2" : "VS AI") : "SOLO",
                 selected ? 0 : 11, 1);
        }
        text(f, 6, 113, a->players[0].ready ? "WAITING FOR P2" : "READY ON YOUR PHONE", 4, 1);
        return;
    }
    if (a->game == ARCADE_TETRIS)
        render_tetris(a, f);
    else if (a->game == ARCADE_PONG)
        render_pong(a, f);
    else
        render_pacman(a, f);
    if (a->stage == ARCADE_COUNTDOWN) {
        rect(f, 47, 48, 66, 33, 0);
        text(f, 53, 51, "READY", 2, 1);
        number(f, 74, 64, (unsigned)((a->phase_until - a->now + 999) / 1000), 1, 2);
    }
    if (a->stage == ARCADE_RESULT) {
        rect(f, 5, 48, 150, 35, 0);
        text(f, 11, 55, a->reason, 2, 1);
        text(f, 20, 70, "BACK TO MENU SOON", 1, 1);
    }
}
