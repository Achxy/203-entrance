#pragma once
#include "core.h"

typedef struct {
    char token[65], csrf[33];
    int64_t expires;
} auth_session_t;
typedef struct {
    auth_session_t sessions[2];
    unsigned failures;
    int64_t failure_window;
} auth_state_t;

bool auth_throttled(auth_state_t *state, int64_t now);
void auth_failed(auth_state_t *state);
int auth_issue(auth_state_t *state, int64_t now, const char token[65], const char csrf[33]);
int auth_validate(const auth_state_t *state, int64_t now, const char *token, const char *csrf,
                  bool cookie_mutation);
void auth_revoke(auth_state_t *state, int index);
