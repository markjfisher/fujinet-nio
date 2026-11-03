/* Host test of the packet-link controller against a scripted peer. */
#include "packet_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static uint8_t sent[2 * PL_MAX_BODY];
static size_t sent_len;
static int done_calls;
static enum pl_outcome last_outcome;
static uint8_t last_body[PL_MAX_BODY];
static size_t last_len;

static void on_send(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    memcpy(sent + sent_len, data, len);
    sent_len += len;
}

static void on_done(void *ctx, enum pl_outcome outcome, const uint8_t *body,
                    size_t len) {
    (void)ctx;
    ++done_calls;
    last_outcome = outcome;
    last_len = len;
    if (len)
        memcpy(last_body, body, len);
}

/* Take the one record the controller sent, checking its framing. */
static void take_record(uint8_t *kind, uint8_t *gen, uint8_t *body,
                        size_t *len) {
    CHECK(sent_len >= PL_HEADER + PL_CRC);
    CHECK(sent[0] == 0xF5 && sent[1] == 0x4E);
    *kind = sent[2];
    *gen = sent[3];
    *len = sent[4] | (size_t)sent[5] << 8;
    CHECK(sent_len == PL_HEADER + *len + PL_CRC);
    const uint8_t *c = sent + PL_HEADER + *len;
    CHECK(pl_crc32(sent + 2, PL_HEADER - 2 + *len, 0) ==
          (c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 |
           (uint32_t)c[3] << 24));
    if (body)
        memcpy(body, sent + PL_HEADER, *len);
    sent_len = 0;
}

static void peer(struct pl_link *l, uint8_t kind, uint8_t gen,
                 const uint8_t *body, size_t len, uint32_t now) {
    uint8_t rec[PL_HEADER + PL_MAX_BODY + PL_CRC];
    pl_receive(l, rec, pl_encode(rec, kind, gen, body, len), now);
}

static uint8_t sync_up(struct pl_link *l, uint32_t *now) {
    uint8_t kind, gen, body[8];
    size_t len;
    pl_poll(l, *now);
    CHECK(sent_len == 0); /* quiet before the Sync */
    *now += 200;
    pl_poll(l, *now);
    take_record(&kind, &gen, body, &len);
    CHECK(kind == PL_KIND_SYNC && len == 1 && body[0] == PL_VERSION);
    const uint8_t ack[] = {1, 0x00, 0x10, 100,
                           0}; /* v1, capacity 4096, 100 ms */
    peer(l, PL_KIND_SYNC_ACK, gen, ack, sizeof ack, *now);
    CHECK(l->state != PL_AWAIT_ACK && l->capacity == 4096 &&
          l->peer_timeout_ms == 100);
    return gen;
}

int main(void) {
    /* zlib's crc32("123456789") */
    CHECK(pl_crc32((const uint8_t *)"123456789", 9, 0) == 0xCBF43926u);
    /* Same bytes as fujinet_tools.packetlink.encode(0x10, 1, b"\x01"). */
    uint8_t rec[16];
    const uint8_t v1 = 1, want[] = {0xF5, 0x4E, 0x10, 0x01, 0x01, 0x00,
                                    0x01, 0x5b, 0x5d, 0xbb, 0x68};
    CHECK(pl_encode(rec, PL_KIND_SYNC, 1, &v1, 1) == sizeof want &&
          !memcmp(rec, want, sizeof want));

    struct pl_link l = {.send = on_send, .done = on_done};
    pl_init(&l);
    uint32_t now = 1000;
    const uint8_t req[] = {0x45, 0x01, 0x06, 0x00, 0x4c, 0x00},
                  resp[] = {0x45, 0x01, 9, 9};
    uint8_t kind, gen, body[64];
    size_t len;

    pl_request(&l, req, sizeof req, now);
    CHECK(done_calls == 1 && last_outcome == PL_NO_PEER); /* port not open */

    pl_open(&l, now);
    uint8_t g = sync_up(&l, &now);

    /* An answer in the generation completes the request; noise is skipped. */
    pl_request(&l, req, sizeof req, now);
    take_record(&kind, &gen, body, &len);
    CHECK(kind == PL_KIND_PACKET && gen == g && len == sizeof req &&
          !memcmp(body, req, len));
    const uint8_t noise[] = {0x00, 0xF5, 0x12, 0xF5};
    pl_receive(&l, noise, sizeof noise, now);
    peer(&l, PL_KIND_PACKET, (uint8_t)(g + 1), resp, sizeof resp,
         now); /* wrong generation */
    CHECK(done_calls == 1);
    peer(&l, PL_KIND_PACKET, g, resp, sizeof resp, now);
    CHECK(done_calls == 2 && last_outcome == PL_ANSWERED &&
          last_len == sizeof resp && !memcmp(last_body, resp, sizeof resp));

    /* A record split across reads, its CRC damaged first. */
    pl_request(&l, req, sizeof req, now);
    take_record(&kind, &gen, NULL, &len);
    uint8_t r[32];
    size_t n = pl_encode(r, PL_KIND_PACKET, g, resp, sizeof resp);
    r[n - 1] ^= 1;
    pl_receive(&l, r, n, now);
    CHECK(done_calls == 2 && l.count.corrupt == 1);
    r[n - 1] ^= 1;
    pl_receive(&l, r, 3, now);
    pl_receive(&l, r + 3, n - 3, now);
    CHECK(done_calls == 3 && last_outcome == PL_ANSWERED);

    /* Corrupt from the peer: sent again, unchanged. */
    pl_request(&l, req, sizeof req, now);
    take_record(&kind, &gen, NULL, &len);
    uint8_t code = PL_ERR_CORRUPT;
    peer(&l, PL_KIND_ERROR, g, &code, 1, now);
    take_record(&kind, &gen, body, &len);
    CHECK(kind == PL_KIND_PACKET && gen == g && !memcmp(body, req, sizeof req));
    peer(&l, PL_KIND_PACKET, g, resp, sizeof resp, now);
    CHECK(done_calls == 4 && last_outcome == PL_ANSWERED);

    /* NotSynchronised (NIO restarted): resync, then the same request again. */
    pl_request(&l, req, sizeof req, now);
    take_record(&kind, &gen, NULL, &len);
    code = PL_ERR_NOT_SYNCHRONISED;
    peer(&l, PL_KIND_ERROR, g, &code, 1, now);
    CHECK(done_calls == 4 && l.state == PL_QUIET);
    uint8_t g2 = sync_up(&l, &now);
    CHECK(g2 != g);
    take_record(&kind, &gen, body, &len);
    CHECK(kind == PL_KIND_PACKET && gen == g2 &&
          !memcmp(body, req, sizeof req));
    peer(&l, PL_KIND_PACKET, g2, resp, sizeof resp, now);
    CHECK(done_calls == 5 && last_outcome == PL_ANSWERED);

    /* Unanswered may have run: reported, never resent, new generation. */
    pl_request(&l, req, sizeof req, now);
    take_record(&kind, &gen, NULL, &len);
    code = PL_ERR_UNANSWERED;
    peer(&l, PL_KIND_ERROR, g2, &code, 1, now);
    CHECK(done_calls == 6 && last_outcome == PL_UNANSWERED &&
          l.state == PL_QUIET);
    uint8_t g3 = sync_up(&l, &now);
    CHECK(sent_len == 0);

    /* Over the peer's capacity: refused without sending. */
    static uint8_t big[4097];
    pl_request(&l, big, sizeof big, now);
    CHECK(done_calls == 7 && last_outcome == PL_REFUSED && sent_len == 0);

    /* No answer in time: timed out, then a fresh generation. */
    pl_request(&l, req, sizeof req, now);
    take_record(&kind, &gen, NULL, &len);
    now += l.answer_timeout_ms;
    pl_poll(&l, now);
    CHECK(done_calls == 8 && last_outcome == PL_TIMED_OUT &&
          l.state == PL_QUIET);
    sync_up(&l, &now);
    CHECK(l.generation != g3);

    /* A peer that never acknowledges: the request fails, the pause grows. */
    pl_open(&l, now);
    pl_request(&l, req, sizeof req, now);
    uint32_t quiet0 = l.quiet_ms;
    for (int i = 0; i < 100 && done_calls == 8; ++i) {
        now += 100;
        pl_poll(&l, now);
        sent_len = 0;
    }
    CHECK(done_calls == 9 && last_outcome == PL_NO_PEER && l.quiet_ms > quiet0);

    pl_close(&l);
    CHECK(l.state == PL_OFF);
    printf(failures ? "packet_link: %d failures\n" : "packet_link: ok\n",
           failures);
    return failures != 0;
}
