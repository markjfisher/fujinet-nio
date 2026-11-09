#pragma once
#include "packet_link.h"

/* Hold complete records across short/nonblocking USB writes. No heap. */
struct probe_queue {
    uint8_t data[2 * (PL_MAX_BODY + PL_HEADER + PL_CRC)];
    size_t head, used;
};
bool probe_append(struct probe_queue *q, const uint8_t *data, size_t len);
size_t probe_front(const struct probe_queue *q, const uint8_t **data);
bool probe_consume(struct probe_queue *q, size_t len);
bool probe_raw_valid(const uint8_t *data, size_t len);
/* Decode the canonical one-U8-status Clock.GetTime response. */
bool probe_clock(const uint8_t *data, size_t len, uint8_t *status,
                 uint64_t *seconds);
extern const uint8_t probe_clock_request[6];

enum probe_bulk_stage { BULK_IDLE, BULK_STAT, BULK_WRITE, BULK_READ, BULK_DONE, BULK_FAILED };
struct probe_bulk {
    enum probe_bulk_stage stage;
    char uri[64];
    uint16_t capacity, size, offset, chunk;
};
bool probe_bulk_start(struct probe_bulk *b, uint16_t capacity, const char *uri);
size_t probe_bulk_request(struct probe_bulk *b, uint8_t *out, size_t limit);
bool probe_bulk_response(struct probe_bulk *b, const uint8_t *p, size_t n);
