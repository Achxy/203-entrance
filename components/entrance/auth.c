#include "auth.h"
#include <string.h>

bool auth_throttled(auth_state_t *state, int64_t now)
{
    if (now - state->failure_window >= 60000000) {
        state->failure_window = now;
        state->failures = 0;
    }
    return state->failures >= 5;
}
void auth_failed(auth_state_t *state)
{
    if (state->failures < 5)
        state->failures++;
}
int auth_issue(auth_state_t *state, int64_t now, const char token[65], const char csrf[33])
{
    int index = state->sessions[0].expires <= state->sessions[1].expires ? 0 : 1;
    memcpy(state->sessions[index].token, token, 65);
    memcpy(state->sessions[index].csrf, csrf, 33);
    state->sessions[index].expires = now + 3600000000LL;
    return index;
}
int auth_validate(const auth_state_t *state, int64_t now, const char *token, const char *csrf,
                  bool cookie_mutation)
{
    if (!token || strlen(token) != 64)
        return -1;
    if (cookie_mutation && (!csrf || strlen(csrf) != 32))
        return -1;
    for (int i = 0; i < 2; i++) {
        if (state->sessions[i].expires > now &&
            constant_equal(token, state->sessions[i].token, 64) &&
            (!cookie_mutation || constant_equal(csrf, state->sessions[i].csrf, 32)))
            return i;
    }
    return -1;
}
void auth_revoke(auth_state_t *state, int index)
{
    if (index >= 0 && index < 2)
        memset(&state->sessions[index], 0, sizeof(state->sessions[index]));
}
