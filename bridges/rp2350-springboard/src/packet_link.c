#include "packet_link.h"
#include <string.h>

#define DEFAULT_RECORD_TIMEOUT_MS 100
#define MAX_QUIET_MS 5000
#define SYNC_ATTEMPT_MS 500
#define MAX_RESENDS 2

static uint32_t crc_table[256];

uint32_t pl_crc32(const uint8_t *data, size_t len, uint32_t crc) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    crc = ~crc;
    while (len--)
        crc = crc_table[(crc ^ *data++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static void put_crc(uint8_t *out, uint32_t crc) {
    for (int i = 0; i < 4; ++i)
        out[i] = (uint8_t)(crc >> 8 * i);
}

size_t pl_encode(uint8_t *out, uint8_t kind, uint8_t generation,
                 const uint8_t *body, size_t len) {
    out[0] = 0xF5;
    out[1] = 0x4E;
    out[2] = kind;
    out[3] = generation;
    out[4] = (uint8_t)len;
    out[5] = (uint8_t)(len >> 8);
    if (len)
        memcpy(out + PL_HEADER, body, len);
    put_crc(out + PL_HEADER + len, pl_crc32(out + 2, PL_HEADER - 2 + len, 0));
    return PL_HEADER + len + PL_CRC;
}

/* Header, body and CRC go out separately so a large body is never copied. */
static void emit(struct pl_link *l, uint8_t kind, const uint8_t *body,
                 size_t len) {
    uint8_t head[PL_HEADER] = {
        0xF5, 0x4E, kind, l->generation, (uint8_t)len, (uint8_t)(len >> 8)};
    uint8_t crc[PL_CRC];
    put_crc(crc, pl_crc32(body, len, pl_crc32(head + 2, PL_HEADER - 2, 0)));
    l->send(l->ctx, head, sizeof head);
    if (len)
        l->send(l->ctx, body, len);
    l->send(l->ctx, crc, sizeof crc);
}

static void finish(struct pl_link *l, enum pl_outcome outcome,
                   const uint8_t *body, size_t len) {
    l->pending = false;
    l->req = NULL;
    l->done(l->ctx, outcome, body, len);
}

/* Quiet first, so the peer abandons anything it half received. */
static void resync(struct pl_link *l, uint32_t now) {
    l->state = PL_QUIET;
    l->since = now;
    l->version = 0;
    l->capacity = 0;
}

static void send_request(struct pl_link *l, uint32_t now) {
    ++l->count.requests;
    emit(l, PL_KIND_PACKET, l->req, l->req_len);
    l->state = PL_AWAIT_ANSWER;
    l->until = now + l->answer_timeout_ms;
}

void pl_init(struct pl_link *l) {
    l->state = PL_OFF;
    l->generation = 0;
    l->quiet_ms = DEFAULT_RECORD_TIMEOUT_MS * 6 / 5;
    l->pending = false;
    l->rx_len = 0;
    if (!l->answer_timeout_ms)
        l->answer_timeout_ms = 30000;
    if (!l->sync_give_up_ms)
        l->sync_give_up_ms = 3000;
    memset(&l->count, 0, sizeof l->count);
}

void pl_open(struct pl_link *l, uint32_t now) {
    l->rx_len = 0;
    resync(l, now);
}

void pl_close(struct pl_link *l) {
    l->state = PL_OFF;
    if (l->pending)
        finish(l, PL_NO_PEER, NULL, 0);
}

void pl_request(struct pl_link *l, const uint8_t *pkt, size_t len,
                uint32_t now) {
    if (l->state == PL_OFF)
        return l->done(l->ctx, PL_NO_PEER, NULL, 0);
    if (!len || len > PL_MAX_BODY || (l->capacity && len > l->capacity))
        return l->done(l->ctx, PL_REFUSED, NULL, 0);
    l->req = pkt;
    l->req_len = len;
    l->pending = true;
    l->pending_since = now;
    l->resends = 0;
    if (l->state == PL_READY)
        send_request(l, now);
}

void pl_cancel(struct pl_link *l, uint32_t now) {
    if (!l->pending)
        return;
    bool in_flight = l->state == PL_AWAIT_ANSWER;
    finish(l, PL_CANCELLED, NULL, 0);
    if (in_flight)
        resync(l, now);
}

static void sync_failed(struct pl_link *l, uint32_t now) {
    l->quiet_ms =
        l->quiet_ms * 2 > MAX_QUIET_MS ? MAX_QUIET_MS : l->quiet_ms * 2;
    resync(l, now);
}

static void on_sync_ack(struct pl_link *l, const uint8_t *body, size_t len,
                        uint32_t now) {
    /* version, capacity (u16), record timeout (u16); later fields ignored. */
    if (len < 5 || !body[0] || body[0] > PL_VERSION)
        return sync_failed(l, now);
    l->version = body[0];
    l->capacity = (uint16_t)(body[1] | body[2] << 8);
    l->peer_timeout_ms = (uint16_t)(body[3] | body[4] << 8);
    uint32_t quiet = l->peer_timeout_ms * 6u / 5 + 1;
    l->quiet_ms = quiet > MAX_QUIET_MS ? MAX_QUIET_MS : quiet;
    l->state = PL_READY;
    if (!l->pending)
        return;
    if (l->req_len > l->capacity) {
        ++l->count.refused;
        return finish(l, PL_REFUSED, NULL, 0);
    }
    send_request(l, now);
}

static void on_error(struct pl_link *l, uint8_t code, uint32_t now) {
    switch (code) {
    case PL_ERR_UNANSWERED:
        ++l->count.unanswered;
        finish(l, PL_UNANSWERED, NULL, 0);
        return resync(l, now);
    case PL_ERR_NOT_SYNCHRONISED:
        /* NIO restarted: the request never ran, so it may go again. */
        resync(l, now);
        if (l->resends++ < MAX_RESENDS) {
            ++l->count.resends;
            l->pending_since = now;
            return;
        }
        break;
    case PL_ERR_CORRUPT:
        if (l->resends++ < MAX_RESENDS) {
            ++l->count.resends;
            return send_request(l, now);
        }
        l->state = PL_READY;
        break;
    default:
        l->state = PL_READY;
        break;
    }
    ++l->count.refused;
    finish(l, PL_REFUSED, NULL, 0);
}

static void on_record(struct pl_link *l, uint8_t kind, uint8_t gen,
                      const uint8_t *body, size_t len, uint32_t now) {
    if (gen != l->generation)
        return;
    if (l->state == PL_AWAIT_ACK) {
        if (kind == PL_KIND_SYNC_ACK)
            on_sync_ack(l, body, len, now);
        else if (kind == PL_KIND_ERROR)
            sync_failed(l, now);
    } else if (l->state == PL_AWAIT_ANSWER) {
        if (kind == PL_KIND_PACKET) {
            ++l->count.answers;
            l->state = PL_READY;
            finish(l, PL_ANSWERED, body, len);
        } else if (kind == PL_KIND_ERROR) {
            on_error(l, len ? body[0] : 0, now);
        }
    }
}

static void drop(struct pl_link *l, size_t n) {
    l->rx_len -= n;
    memmove(l->rx, l->rx + n, l->rx_len);
}

/* Handle every whole record in rx, skipping noise and damaged records. */
static void extract(struct pl_link *l, uint32_t now) {
    for (;;) {
        size_t at = 0;
        while (at + 1 < l->rx_len &&
               !(l->rx[at] == 0xF5 && l->rx[at + 1] == 0x4E))
            ++at;
        if (at + 1 >= l->rx_len) {
            drop(l, l->rx_len && l->rx[l->rx_len - 1] == 0xF5 ? l->rx_len - 1
                                                              : l->rx_len);
            return;
        }
        drop(l, at);
        if (l->rx_len < PL_HEADER)
            return;
        size_t size = l->rx[4] | (size_t)l->rx[5] << 8;
        if (size > PL_MAX_BODY) {
            drop(l, 2);
            continue;
        }
        if (l->rx_len < PL_HEADER + size + PL_CRC)
            return;
        const uint8_t *c = l->rx + PL_HEADER + size;
        uint32_t crc = c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 |
                       (uint32_t)c[3] << 24;
        if (crc != pl_crc32(l->rx + 2, PL_HEADER - 2 + size, 0)) {
            ++l->count.corrupt;
            drop(l, 2);
            continue;
        }
        on_record(l, l->rx[2], l->rx[3], l->rx + PL_HEADER, size, now);
        drop(l, PL_HEADER + size + PL_CRC);
    }
}

void pl_receive(struct pl_link *l, const uint8_t *data, size_t len,
                uint32_t now) {
    while (len) {
        size_t n = sizeof l->rx - l->rx_len;
        if (n > len)
            n = len;
        memcpy(l->rx + l->rx_len, data, n);
        l->rx_len += n;
        data += n;
        len -= n;
        extract(l, now);
    }
}

void pl_poll(struct pl_link *l, uint32_t now) {
    switch (l->state) {
    case PL_QUIET:
        if (now - l->since < l->quiet_ms)
            break;
        if (++l->generation == 0)
            l->generation = 1;
        ++l->count.syncs;
        l->rx_len = 0;
        {
            uint8_t version = PL_VERSION;
            emit(l, PL_KIND_SYNC, &version, 1);
        }
        l->state = PL_AWAIT_ACK;
        l->until = now + (2 * l->quiet_ms > SYNC_ATTEMPT_MS ? 2 * l->quiet_ms
                                                            : SYNC_ATTEMPT_MS);
        break;
    case PL_AWAIT_ACK:
        if ((int32_t)(now - l->until) < 0)
            break;
        ++l->count.sync_timeouts;
        sync_failed(l, now);
        break;
    case PL_READY:
        if (l->pending)
            send_request(l, now);
        break;
    case PL_AWAIT_ANSWER:
        if ((int32_t)(now - l->until) < 0)
            break;
        ++l->count.timeouts;
        finish(l, PL_TIMED_OUT, NULL, 0);
        resync(l, now);
        break;
    case PL_OFF:
        break;
    }
    if (l->pending && (l->state == PL_QUIET || l->state == PL_AWAIT_ACK) &&
        now - l->pending_since >= l->sync_give_up_ms)
        finish(l, PL_NO_PEER, NULL, 0);
}

const char *pl_state_name(enum pl_state s) {
    static const char *const names[] = {"off", "quiet", "syncing", "ready",
                                        "waiting"};
    return s <= PL_AWAIT_ANSWER ? names[s] : "?";
}
