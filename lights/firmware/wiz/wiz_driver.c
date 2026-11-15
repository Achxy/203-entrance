#include "wiz_driver.h"
#include <string.h>

wiz_error_t wiz_send_guard_check(wiz_send_guard_t *g, uint64_t now)
{
    if (!g || g->transmitted || g->checked) return WIZ_ARGUMENT;
    if (now < g->last_now_ms) return WIZ_CLOCK_ROLLBACK;
    g->last_now_ms = now;
    if (now >= g->deadline_ms) return WIZ_TIMEOUT;
    if (g->desired && (!g->authorize || !g->command_id ||
        !g->authorize(g->authorize_context, g->index, g->command_id, g->desired, now)))
        return WIZ_CANCELLED;
    g->checked = true;
    return WIZ_OK;
}

bool wiz_peer_allowed(const wiz_peer_t *p, const uint8_t mac[6])
{
    if (!p || !mac || memcmp(p->mac, mac, 6)) return false;
    uint32_t inverse = ~p->netmask;
    if (inverse < 3 || inverse > UINT32_C(0x00ffffff) || (inverse & (inverse + 1))) return false;
    /* Provisioned private APs only. This layer cannot be used as a public UDP
     * proxy, even if an integration accidentally hands it a campus address. */
    bool private_ap = (p->ap_ip >> 24) == 10 || (p->ap_ip >> 20) == 0xac1 ||
                      (p->ap_ip >> 16) == 0xc0a8;
    bool private_peer = (p->ip >> 24) == 10 || (p->ip >> 20) == 0xac1 ||
                        (p->ip >> 16) == 0xc0a8;
    if (!private_ap || !private_peer || p->ip == p->ap_ip ||
        (p->ip & p->netmask) != (p->ap_ip & p->netmask)) return false;
    uint32_t host = p->ip & inverse;
    uint32_t ap_host = p->ap_ip & inverse;
    return host != 0 && host != inverse && ap_host != 0 && ap_host != inverse;
}

wiz_error_t wiz_driver_init(wiz_driver_t *d, const wiz_io_t *io)
{
    if (!d || !io || !io->resolve || !io->open || !io->send ||
        !io->receive || !io->close) return WIZ_ARGUMENT;
    memset(d, 0, sizeof(*d));
    d->io = *io;
    return WIZ_OK;
}

static bool valid_raw_mac(const uint8_t mac[6])
{
    if (!mac || (mac[0] & 1u)) return false;
    unsigned any = 0;
    for (unsigned n = 0; n < 6; ++n) any |= mac[n];
    return any != 0;
}

wiz_error_t wiz_driver_enroll(wiz_driver_t *d, uint8_t index, const uint8_t mac[6],
                              const wiz_security_t *security)
{
    if (!d || index >= WIZ_MAX_BULBS || !valid_raw_mac(mac) || !security) return WIZ_ARGUMENT;
    if (d->active) return WIZ_BUSY;
    if (security->signing_required && !security->key.present) return WIZ_KEY_REQUIRED;
    for (unsigned i = 0; i < WIZ_MAX_BULBS; ++i)
        if (i != index && d->bulbs[i].enrolled && !memcmp(d->bulbs[i].mac, mac, 6)) return WIZ_ARGUMENT;
    /* The caller may deliberately re-enroll using this slot's existing values. */
    wiz_security_t copy = *security;
    uint8_t enrolled_mac[6];
    memcpy(enrolled_mac, mac, 6);
    wiz_key_clear(&d->bulbs[index].security.key);
    memset(&d->bulbs[index], 0, sizeof(d->bulbs[index]));
    d->bulbs[index].enrolled = true;
    memcpy(d->bulbs[index].mac, enrolled_mac, 6);
    d->bulbs[index].security = copy;
    wiz_key_clear(&copy.key);
    return WIZ_OK;
}

wiz_error_t wiz_driver_remove(wiz_driver_t *d, uint8_t index)
{
    if (!d || index >= WIZ_MAX_BULBS) return WIZ_ARGUMENT;
    if (d->active) return WIZ_BUSY;
    wiz_key_clear(&d->bulbs[index].security.key);
    memset(&d->bulbs[index], 0, sizeof(d->bulbs[index]));
    return WIZ_OK;
}

wiz_error_t wiz_driver_begin(wiz_driver_t *d, uint8_t index, wiz_method_t method,
                             const wiz_patch_t *patch, const uint8_t command_id[16],
                             uint64_t now, uint64_t deadline)
{
    if (!d || index >= WIZ_MAX_BULBS || !wiz_method_name(method)) return WIZ_ARGUMENT;
    if (d->active) return WIZ_BUSY;
    if (now < d->last_now_ms) return WIZ_CLOCK_ROLLBACK;
    if (deadline <= now || deadline - now > WIZ_MAX_OPERATION_MS) return WIZ_ARGUMENT;
    if (!d->bulbs[index].enrolled) return WIZ_NOT_ENROLLED;
    if (method == WIZ_SET_PILOT) {
        if (!d->io.authorize || !command_id) return WIZ_ARGUMENT;
        wiz_error_t e = wiz_validate_patch(patch, &d->bulbs[index].capabilities);
        if (e != WIZ_OK) return e;
    } else if (patch || command_id) return WIZ_ARGUMENT;
    memset(&d->desired, 0, sizeof(d->desired));
    memset(&d->last_observation, 0, sizeof(d->last_observation));
    memset(d->command_id, 0, sizeof(d->command_id));
    if (patch) d->desired = *patch;
    if (command_id) memcpy(d->command_id, command_id, 16);
    d->index = index;
    d->requested_method = d->wire_method = method;
    d->sends = 0;
    d->last_now_ms = now;
    d->deadline_ms = deadline;
    d->next_send_ms = now;
    d->have_peer = d->opened = d->write_sent = d->acknowledged = d->have_observation = false;
    d->active = true;
    return WIZ_OK;
}

static wiz_error_t finish(wiz_driver_t *d, wiz_error_t error,
                           const wiz_reply_t *reply, wiz_completion_t *out)
{
    memset(out, 0, sizeof(*out));
    out->index = d->index;
    out->requested_method = d->requested_method;
    out->error = error;
    out->write_sent = d->write_sent;
    out->acknowledged = d->acknowledged;
    memcpy(out->command_id, d->command_id, sizeof(out->command_id));
    if (!reply && d->have_observation) reply = &d->last_observation;
    if (reply) {
        out->reply = *reply;
        out->observed = reply->method == WIZ_GET_PILOT && reply->state.fields != 0;
        out->matches = d->requested_method == WIZ_SET_PILOT && out->observed &&
                       wiz_state_matches(&d->desired, &reply->state);
    }
    if (d->opened) d->io.close(d->io.context);
    d->active = d->opened = false;
    return WIZ_OK;
}

static void begin_readback(wiz_driver_t *d, uint64_t now)
{
    d->wire_method = WIZ_GET_PILOT;
    d->sends = 0;
    d->next_send_ms = now;
}

wiz_error_t wiz_driver_poll(wiz_driver_t *d, uint64_t now,
                            wiz_wall_clock_t clock, wiz_completion_t *out)
{
    if (!d || !out) return WIZ_ARGUMENT;
    if (!d->active) return WIZ_IDLE;
    if (now < d->last_now_ms) return finish(d, WIZ_CLOCK_ROLLBACK, NULL, out);
    d->last_now_ms = now;
    if (now >= d->deadline_ms) return finish(d, WIZ_TIMEOUT, NULL, out);
    wiz_bulb_t *bulb = &d->bulbs[d->index];
    /* Check association/lease on every poll too: an ACK from an old DHCP owner
     * must not survive an IP reassignment while waiting for a reply. */
    wiz_peer_t peer;
    wiz_error_t e = d->io.resolve(d->io.context, bulb->mac, &peer);
    if (e != WIZ_OK || !wiz_peer_allowed(&peer, bulb->mac))
        return finish(d, e == WIZ_OK ? WIZ_NO_PEER : e, NULL, out);
    if (d->have_peer && (peer.ip != d->peer.ip || peer.ap_ip != d->peer.ap_ip ||
                        peer.netmask != d->peer.netmask))
        return finish(d, WIZ_PEER_CHANGED, NULL, out);
    d->peer = peer;
    d->have_peer = true;
    if (d->requested_method == WIZ_SET_PILOT &&
        !d->io.authorize(d->io.context, d->index, d->command_id, &d->desired, now))
        return finish(d, WIZ_CANCELLED, NULL, out);
    /* Bound per-poll work even if a hostile station floods our socket. */
    if (d->opened) for (unsigned packets = 0; packets < 4; ++packets) {
        size_t size = 0;
        uint32_t ip = 0;
        uint16_t port = 0;
        e = d->io.receive(d->io.context, d->packet.rx, sizeof(d->packet.rx), &size, &ip, &port);
        if (e == WIZ_IDLE) break;
        if (e != WIZ_OK && e != WIZ_TOO_LARGE) return finish(d, e, NULL, out);
        if (ip != d->peer.ip || port != WIZ_PORT) continue;
        if (e == WIZ_TOO_LARGE || size > WIZ_DATAGRAM_MAX)
            return finish(d, WIZ_TOO_LARGE, NULL, out);
        if (e != WIZ_OK) return finish(d, e, NULL, out);
        if (!d->sends) continue;
        wiz_reply_t reply;
        e = wiz_parse_reply(d->packet.rx, size, d->wire_method, bulb->mac, &reply);
        if (e == WIZ_WRONG_METHOD || e == WIZ_WRONG_IDENTITY || e == WIZ_MALFORMED) continue;
        if (e != WIZ_OK) return finish(d, e, &reply, out);
        if (d->wire_method == WIZ_SET_PILOT) {
            d->acknowledged = reply.accepted;
            begin_readback(d, now);
            break;
        }
        wiz_capabilities_merge(&bulb->capabilities, &reply);
        if (d->requested_method == WIZ_SET_PILOT && !wiz_state_matches(&d->desired, &reply.state)) {
            /* A fade can still be settling. Retry only the observation, inside
             * the original deadline; do not re-apply an acknowledged write. */
            d->last_observation = reply;
            d->have_observation = true;
            if (d->sends >= WIZ_MAX_SENDS) return finish(d, WIZ_UNCONFIRMED, &reply, out);
            break;
        }
        return finish(d, WIZ_OK, &reply, out);
    }
    if (now < d->next_send_ms) return WIZ_BUSY;
    if (d->sends >= WIZ_MAX_SENDS) {
        /* A lost ACK never causes an unbounded write retry. Read the state;
         * reaching the desired state does not prove which sender caused it. */
        if (d->wire_method == WIZ_SET_PILOT) begin_readback(d, now);
        else return finish(d, d->have_observation ? WIZ_UNCONFIRMED : WIZ_TIMEOUT, NULL, out);
    }
    if (d->requested_method == WIZ_SET_PILOT &&
        !d->io.authorize(d->io.context, d->index, d->command_id, &d->desired, now))
        return finish(d, WIZ_CANCELLED, NULL, out);
    const wiz_patch_t *patch = d->wire_method == WIZ_SET_PILOT ? &d->desired : NULL;
    e = wiz_encode_request(d->wire_method, patch, &bulb->capabilities,
                           &bulb->security, clock, d->packet.tx, sizeof(d->packet.tx), &d->tx_size);
    if (e != WIZ_OK) return finish(d, e, NULL, out);
    if (!d->opened) {
        e = d->io.open(d->io.context, &d->peer);
        if (e != WIZ_OK) return finish(d, e, NULL, out);
        d->opened = true;
    }
    wiz_send_guard_t guard = {.authorize_context = d->io.context,
        .authorize = d->io.authorize, .command_id = d->command_id,
        .desired = d->requested_method == WIZ_SET_PILOT ? &d->desired : NULL,
        .last_now_ms = now, .deadline_ms = d->deadline_ms, .index = d->index};
    e = d->io.send(d->io.context, &d->peer, (const uint8_t *)d->packet.tx, d->tx_size, &guard);
    d->last_now_ms = guard.last_now_ms;
    /* Preserve physical-send evidence even if outer bookkeeping subsequently
     * fails; a local error cannot retract a datagram already handed to lwIP. */
    if (guard.transmitted && d->wire_method == WIZ_SET_PILOT) d->write_sent = true;
    if (e == WIZ_OK && (!guard.checked || !guard.transmitted)) e = WIZ_IO;
    if (e != WIZ_OK) return finish(d, e, NULL, out);
    ++d->sends;
    d->next_send_ms = d->last_now_ms > UINT64_MAX - WIZ_RETRY_MS ? UINT64_MAX :
                      d->last_now_ms + WIZ_RETRY_MS;
    return WIZ_BUSY;
}

wiz_error_t wiz_driver_cancel(wiz_driver_t *d, wiz_completion_t *out)
{
    if (!d || !out) return WIZ_ARGUMENT;
    if (!d->active) return WIZ_IDLE;
    return finish(d, WIZ_CANCELLED, NULL, out);
}
