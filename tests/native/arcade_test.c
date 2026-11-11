#include "arcade_core.h"
#include "arcade_address.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void addresses(void)
{
    struct sockaddr_storage storage = {0};
    uint32_t ip = 0;
    struct sockaddr_in *v4 = (struct sockaddr_in *)&storage;
    v4->sin_family = AF_INET;
    v4->sin_addr.s_addr = htonl(0xc0a80401u);
    assert(arcade_address_ipv4(&storage, sizeof(*v4), &ip) && ip == htonl(0xc0a80401u));
    assert(!arcade_address_ipv4(&storage, sizeof(*v4) - 1, &ip));
    memset(&storage, 0, sizeof(storage));
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&storage;
    v6->sin6_family = AF_INET6;
    const uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255, 192, 168, 4, 1};
    memcpy(v6->sin6_addr.s6_addr, mapped, 16);
    assert(arcade_address_ipv4(&storage, sizeof(*v6), &ip) && ip == htonl(0xc0a80401u));
    v6->sin6_addr.s6_addr[0] = 0x20;
    assert(!arcade_address_ipv4(&storage, sizeof(*v6), &ip));
}
static void command(arcade_t *a, int slot, enum arcade_action op, int value, int axis)
{
    arcade_player_t *p = &a->players[slot];
    assert(arcade_command(a, p->id, p->sequence + 1, op, value, axis, a->now) == ARCADE_OK);
}
static void keep(arcade_t *a, uint64_t t)
{
    for (int i = 0; i < 2; i++)
        if (a->players[i].id) {
            a->players[i].last_seen = t;
            a->players[i].last_action = t;
        }
    arcade_tick(a, t);
}
static void start(arcade_t *a, int game, int players)
{
    arcade_init(a, 42, 1000);
    assert(arcade_join(a, 100, 1000) == ARCADE_OK);
    if (players == 2)
        assert(arcade_join(a, 200, 1001) == ARCADE_OK);
    command(a, 0, ARCADE_SELECT, game, 0);
    command(a, 0, ARCADE_READY, 1, 0);
    if (players == 2) {
        assert(a->stage == ARCADE_MENU);
        command(a, 1, ARCADE_READY, 1, 0);
    }
    assert(a->stage == ARCADE_COUNTDOWN);
    keep(a, 3300);
    assert(a->stage == ARCADE_PLAY);
}
static void sessions(void)
{
    arcade_t a;
    arcade_init(&a, 1, 1000);
    assert(arcade_join(&a, 0, 1000) == ARCADE_INVALID);
    assert(arcade_join(&a, 11, 1000) == ARCADE_OK);
    assert(a.stage == ARCADE_MENU);
    assert(arcade_join(&a, 22, 1000) == ARCADE_OK);
    assert(a.game == ARCADE_PONG);
    assert(arcade_join(&a, 33, 1000) == ARCADE_BUSY);
    assert(arcade_command(&a, 22, 1, ARCADE_SELECT, 0, 0, 1000) == ARCADE_INVALID);
    assert(arcade_command(&a, 11, 1, ARCADE_SELECT, 0, 0, 1000) == ARCADE_INVALID);
    command(&a, 0, ARCADE_READY, 1, 0);
    assert(a.stage == ARCADE_MENU);
    command(&a, 1, ARCADE_READY, 1, 0);
    assert(a.stage == ARCADE_COUNTDOWN);
    assert(!a.game_state.pong.ai);
    assert(arcade_join(&a, 33, 1100) == ARCADE_BUSY);
    assert(arcade_command(&a, 11, a.players[0].sequence, ARCADE_LEAVE, 0, 0, 1100) == ARCADE_STALE);
    assert(arcade_command(&a, 99, 100, ARCADE_LEAVE, 0, 0, 1100) == ARCADE_NOT_PLAYER);
    assert(arcade_command(&a, 11, 100, ARCADE_INPUT, 0, 1001, 1100) == ARCADE_INVALID);
    command(&a, 0, ARCADE_LEAVE, 0, 0);
    assert(a.players[0].id == 22 && a.stage == ARCADE_MENU);
    command(&a, 0, ARCADE_LEAVE, 0, 0);
    assert(a.stage == ARCADE_IDLE);
    assert(arcade_join(&a, 33, a.now) == ARCADE_BUSY);
    assert(arcade_join(&a, 33, a.now + 10001) == ARCADE_OK);
    uint64_t joined = a.now;
    for (uint64_t t = joined + 1000; t < joined + 25000; t += 1000) {
        if (a.players[0].id)
            assert(arcade_command(&a, 33, (uint32_t)t, ARCADE_HEARTBEAT, 0, 0, t) == ARCADE_OK);
    }
    arcade_tick(&a, joined + 26000);
    assert(a.stage == ARCADE_IDLE);
    start(&a, ARCADE_PONG, 1);
    uint64_t end = a.turn_until;
    for (uint64_t t = a.now + 20; t <= end + 20; t += 20) {
        if (a.players[0].id) {
            a.players[0].last_seen = t;
            a.players[0].last_action = t;
        }
        arcade_tick(&a, t);
    }
    assert(a.stage == ARCADE_IDLE);
    assert(strstr(a.reason, "Turn complete"));
    start(&a, ARCADE_TETRIS, 1);
    arcade_tick(&a, a.now + 10001);
    assert(a.stage == ARCADE_IDLE);
}
static void maze(void)
{
    unsigned char seen[19][25] = {0};
    int qx[475], qy[475], r = 0, w = 1;
    qx[0] = 12;
    qy[0] = 15;
    seen[15][12] = 1;
    while (r < w) {
        int x = qx[r], y = qy[r++];
        int dx[] = {-1, 1, 0, 0}, dy[] = {0, 0, -1, 1};
        for (int d = 0; d < 4; d++) {
            int nx = x + dx[d], ny = y + dy[d];
            if (ny == 9) {
                if (nx < 0)
                    nx = 24;
                if (nx > 24)
                    nx = 0;
            }
            if (!arcade_maze_wall(nx, ny) && !seen[ny][nx]) {
                seen[ny][nx] = 1;
                qx[w] = nx;
                qy[w++] = ny;
            }
        }
    }
    for (int y = 0; y < 19; y++)
        for (int x = 0; x < 25; x++)
            assert(arcade_maze_wall(x, y) || seen[y][x]);
}
static void pong(void)
{
    unsigned house_wins = 0;
    for (int run = 0; run < 200; run++) {
        arcade_t a;
        start(&a, ARCADE_PONG, 1);
        a.rng = run + 1;
        arcade_pong_t *p = &a.game_state.pong;
        /* A perfect-information legal player anticipates the reflected impact,
           so this challenges the house against more than reaction tracking. */
        for (uint64_t t = a.now + 20; t < 200000 && a.stage == ARCADE_PLAY; t += 20) {
            float target = p->y;
            if (p->vx < 0) {
                float y = p->y + p->vy * ((14 - p->x) / p->vx);
                float u = fmodf(y - 22, 192);
                if (u < 0)
                    u += 192;
                target = 22 + (u > 96 ? 192 - u : u);
            }
            int direction = target > p->paddle[0] + .5   ? 1000
                            : target < p->paddle[0] - .5 ? -1000
                                                         : 0;
            a.players[0].axis = direction;
            a.players[0].last_input = t;
            keep(&a, t);
            assert(p->points[0] == 0);
            assert(p->paddle[1] >= 31 && p->paddle[1] <= 109);
        }
        assert(a.stage == ARCADE_RESULT && a.winner == 2);
        house_wins++;
    }
    start(&(arcade_t){0}, ARCADE_PONG, 2);
    printf("House AI: %u/200 bounded perfect-information opponents defeated\n", house_wins);
}
static void pulse(arcade_t *a, unsigned buttons)
{
    keep(a, a->now + 20);
    command(a, 0, ARCADE_INPUT, 0, 0);
    keep(a, a->now + 20);
    command(a, 0, ARCADE_INPUT, buttons, 0);
}
static void tetris(void)
{
    arcade_t a;
    start(&a, ARCADE_TETRIS, 1);
    arcade_tetris_t *t = &a.game_state.tetris;
    unsigned kinds = 0;
    for (int i = 0; i < 7; i++) {
        assert(!(kinds & (1u << t->kind)));
        kinds |= 1u << t->kind;
        pulse(&a, ARCADE_DROP);
        memset(t->cells, 0, sizeof(t->cells));
    }
    assert(kinds == 127);
    uint8_t first = t->kind;
    pulse(&a, ARCADE_HOLD);
    assert(t->held_kind == first && t->hold_used);
    uint8_t current = t->kind;
    pulse(&a, ARCADE_HOLD);
    assert(t->kind == current && t->held_kind == first);
    pulse(&a, ARCADE_DROP);
    assert(!t->hold_used);
    pulse(&a, ARCADE_HOLD);
    assert(t->kind == first);
    start(&a, ARCADE_TETRIS, 1);
    t = &a.game_state.tetris;
    t->kind = 0;
    t->rotation = 0;
    t->x = 0;
    t->y = 18;
    for (int x = 4; x < 10; x++)
        t->cells[21][x] = 1;
    pulse(&a, ARCADE_DROP);
    assert(t->clear_mask == (1u << 21));
    keep(&a, a.now + 200);
    assert(t->lines == 1 && t->score >= 100 && !t->clear_mask);
    for (int x = 0; x < 10; x++)
        assert(t->cells[21][x] == 0);
    t->y = 10;
    for (int y = 0; y < 2; y++)
        for (int x = 3; x < 7; x++)
            t->cells[y][x] = 1;
    pulse(&a, ARCADE_DROP);
    assert(a.stage == ARCADE_RESULT);
}
static void human_pong(void)
{
    for (int winner = 0; winner < 2; winner++) {
        arcade_t a;
        start(&a, ARCADE_PONG, 2);
        arcade_pong_t *p = &a.game_state.pong;
        assert(!p->ai);
        p->points[winner] = 4;
        p->x = winner == 0 ? 157 : 3;
        p->vx = winner == 0 ? 100 : -100;
        p->vy = 0;
        p->serve_at = 0;
        keep(&a, a.now + 20);
        assert(a.stage == ARCADE_RESULT && a.winner == winner);
    }
}
static void pacman(void)
{
    arcade_t a;
    start(&a, ARCADE_PACMAN, 1);
    arcade_pacman_t *p = &a.game_state.pacman;
    for (int life = 2; life >= 0; life--) {
        p->respawn_at = 0;
        p->player = (arcade_actor_t){.x = 12, .y = 15, .direction = 0, .wanted = 0};
        p->ghosts[0] = (arcade_actor_t){.x = 11, .y = 15};
        p->move_at = a.now - 200;
        p->ghost_at = a.now;
        keep(&a, a.now + 20);
        assert(p->lives == life);
    }
    assert(a.stage == ARCADE_RESULT);
    start(&a, ARCADE_PACMAN, 1);
    p = &a.game_state.pacman;
    p->respawn_at = 0;
    p->power_until = a.now + 6000;
    p->player = (arcade_actor_t){.x = 12, .y = 15, .direction = 0, .wanted = 0};
    p->ghosts[0] = (arcade_actor_t){.x = 11, .y = 15};
    p->move_at = a.now - 200;
    p->ghost_at = a.now;
    keep(&a, a.now + 20);
    assert(p->lives == 3 && p->score >= 200 && p->ghosts[0].x == 12 && p->ghosts[0].y == 9);
}
static void malformed_and_random(void)
{
    arcade_t a;
    arcade_init(&a, 15, 1000);
    assert(arcade_join(&a, 1, 1000) == ARCADE_OK);
    assert(arcade_command(&a, 1, 1, (enum arcade_action) - 1, 0, 0, 1000) == ARCADE_INVALID);
    assert(arcade_command(&a, 1, 1, ARCADE_MENU_ACTION, 0, 0, 1000) == ARCADE_INVALID);
    unsigned random = 17;
    uint8_t frame[ARCADE_FRAME_BYTES];
    for (int i = 0; i < 120000; i++) {
        random = random * 1664525u + 1013904223u;
        uint64_t now = a.now + 20;
        if (a.stage == ARCADE_IDLE && now >= a.cooldown_until)
            arcade_join(&a, 1, now);
        if (a.stage == ARCADE_MENU) {
            arcade_command(&a, 1, (uint32_t)i * 3, ARCADE_SELECT, (random >> 16) % 3, 0, now);
            arcade_command(&a, 1, (uint32_t)i * 3 + 1, ARCADE_READY, 1, 0, now);
        }
        arcade_command(&a, 1, (uint32_t)i * 3 + 2, ARCADE_INPUT, (random >> 24) & 127,
                       (int)(random % 2001) - 1000, now);
        if (i % 37 == 0)
            arcade_render(&a, frame);
        assert(a.stage >= ARCADE_IDLE && a.stage <= ARCADE_RESULT);
        assert(a.game >= ARCADE_TETRIS && a.game < ARCADE_GAME_COUNT);
    }
}
static void render_files(const char *dir)
{
    char path[512];
    uint8_t pixels[ARCADE_FRAME_BYTES];
    for (int g = 0; g < 3; g++) {
        arcade_t a;
        start(&a, g, 1);
        for (uint64_t t = a.now + 20; t < 6000; t += 20)
            keep(&a, t);
        arcade_render(&a, pixels);
        snprintf(path, sizeof(path), "%s/game-%d.ppm", dir, g);
        FILE *f = fopen(path, "wb");
        assert(f);
        fprintf(f, "P6\n160 128\n255\n");
        for (int i = 0; i < 160 * 128; i++) {
            uint16_t color = arcade_color(i & 1 ? pixels[i / 2] & 15 : pixels[i / 2] >> 4);
            unsigned char rgb[] = {(color >> 11) * 255 / 31, ((color >> 5) & 63) * 255 / 63,
                                   (color & 31) * 255 / 31};
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
    }
}
int main(int argc, char **argv)
{
    addresses();
    sessions();
    maze();
    pong();
    tetris();
    human_pong();
    pacman();
    malformed_and_random();
    if (argc > 1)
        render_files(argv[1]);
    puts("Arcade sessions, deadlines, role isolation, maze connectivity, rendering passed");
    return 0;
}
