#ifndef LIGHTING_SESSION_WIRE_H
#define LIGHTING_SESSION_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_PACKET_MAX 1024u
#define LS_PLAINTEXT_MAX (LS_PACKET_MAX - 16u)
#define LS_BINDING_CONTEXT_MAX 256u
#define LS_OWNER_USERNAME "203-owner"
#define LS_GUEST_USERNAME "203-guest"

typedef enum { LS_ROLE_NONE = 0, LS_ROLE_GUEST, LS_ROLE_OWNER } ls_role_t;

/* Validate the exact Security2 client message before protobuf-c allocates or
 * performs SRP work. Unknown/duplicate fields and non-minimal varints fail. */
bool ls_client_handshake(const uint8_t *data, size_t size, unsigned step,
                         ls_role_t *credential);

/* Canonical, domain-separated context signed by the gateway's pinned P-256
 * identity. The transcript must be computed from this session by the server. */
size_t ls_binding_context(char out[LS_BINDING_CONTEXT_MAX], const char *gateway,
                          const uint8_t boot[8], const uint8_t transcript[32],
                          const uint8_t challenge[32], ls_role_t credential);
bool ls_lower_hex(const char *text, size_t size, uint8_t *out, size_t capacity);

#endif
