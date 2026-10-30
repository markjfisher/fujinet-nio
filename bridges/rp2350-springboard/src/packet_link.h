/* Controller side of fujinet-nio's packet link (docs/packet_link.md): raw
 * FujiBus packets in CRC-checked records, one request at a time, in a
 * generation agreed with Sync/SyncAck. No SDK calls, so it builds on a host. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PL_VERSION 1
#define PL_HEADER 6
#define PL_CRC 4
#define PL_MAX_BODY 8192

enum pl_kind {
    PL_KIND_PACKET = 0x01,
    PL_KIND_ERROR = 0x02,
    PL_KIND_SYNC = 0x10,
    PL_KIND_SYNC_ACK = 0x11
};
enum pl_error {
    PL_ERR_CORRUPT = 1,
    PL_ERR_OVERSIZED,
    PL_ERR_NOT_SYNCHRONISED,
    PL_ERR_EMPTY,
    PL_ERR_UNANSWERED,
    PL_ERR_UNSUPPORTED_VERSION,
};

/* How a request ended. Unanswered and timed out are unknown completions: the
 * request may have run, so it is never sent again. */
enum pl_outcome {
    PL_ANSWERED,
    PL_REFUSED,
    PL_UNANSWERED,
    PL_TIMED_OUT,
    PL_NO_PEER,
    PL_CANCELLED,
};

enum pl_state { PL_OFF, PL_QUIET, PL_AWAIT_ACK, PL_READY, PL_AWAIT_ANSWER };

struct pl_counters {
    uint32_t syncs, sync_timeouts, requests, answers, refused, unanswered,
        timeouts, resends, corrupt;
};

struct pl_link {
    /* Set before pl_init: send queues bytes for the peer; done reports the
     * request's completion, with the response body when answered. */
    void (*send)(void *ctx, const uint8_t *data, size_t len);
    void (*done)(void *ctx, enum pl_outcome outcome, const uint8_t *body,
                 size_t len);
    void *ctx;
    uint32_t answer_timeout_ms;
    uint32_t sync_give_up_ms;

    enum pl_state state;
    uint8_t generation, version;
    uint16_t capacity, peer_timeout_ms;
    uint32_t quiet_ms, since, until;
    const uint8_t *req;
    size_t req_len;
    bool pending;
    uint32_t pending_since;
    uint8_t resends;
    uint8_t rx[PL_HEADER + PL_MAX_BODY + PL_CRC];
    size_t rx_len;
    struct pl_counters count;
};

uint32_t pl_crc32(const uint8_t *data, size_t len, uint32_t crc);
/* Write one record into out (PL_HEADER + len + PL_CRC bytes); returns its size.
 */
size_t pl_encode(uint8_t *out, uint8_t kind, uint8_t generation,
                 const uint8_t *body, size_t len);

void pl_init(struct pl_link *l);
/* The peer's port opened: start a generation. Closed: fail any request. */
void pl_open(struct pl_link *l, uint32_t now);
void pl_close(struct pl_link *l);
/* Queue one request; pkt must stay valid until done is called. */
void pl_request(struct pl_link *l, const uint8_t *pkt, size_t len,
                uint32_t now);
void pl_cancel(struct pl_link *l, uint32_t now);
void pl_receive(struct pl_link *l, const uint8_t *data, size_t len,
                uint32_t now);
void pl_poll(struct pl_link *l, uint32_t now);
const char *pl_state_name(enum pl_state s);
