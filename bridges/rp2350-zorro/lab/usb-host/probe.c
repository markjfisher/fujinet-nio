#include "probe.h"
#include <string.h>

const uint8_t probe_clock_request[6] = {0x45, 0x01, 0x06, 0x00, 0x4c, 0x00};

bool probe_append(struct probe_queue *q, const uint8_t *data, size_t len) {
    if (len > sizeof q->data - q->used) return false;
    for (size_t i = 0; i < len; ++i)
        q->data[(q->head + q->used + i) % sizeof q->data] = data[i];
    q->used += len;
    return true;
}

size_t probe_front(const struct probe_queue *q, const uint8_t **data) {
    *data = q->data + q->head;
    size_t n = sizeof q->data - q->head;
    return n < q->used ? n : q->used;
}

bool probe_consume(struct probe_queue *q, size_t len) {
    if (len > q->used) return false;
    q->head = (q->head + len) % sizeof q->data;
    q->used -= len;
    return true;
}

bool probe_raw_valid(const uint8_t *p, size_t n) {
    if (n < 6 || n > 65535 || (p[2] | (size_t)p[3] << 8) != n)
        return false;
    unsigned sum = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i != 4) sum += p[i];
        sum = (sum & 255u) + (sum >> 8);
    }
    return (uint8_t)sum == p[4];
}

bool probe_clock(const uint8_t *p, size_t n, uint8_t *status, uint64_t *seconds) {
    if (!probe_raw_valid(p, n) || p[0] != 0x45 || p[1] != 1 ||
        n < 7 || p[5] != 1) return false;
    *status = p[6];
    *seconds = 0;
    if (*status) return n == 7;
    if (n != 19 || p[7] != 1 || p[8] || p[9] || p[10]) return false;
    for (unsigned i = 0; i < 8; ++i) *seconds |= (uint64_t)p[11+i] << (8*i);
    return true;
}

/* FileDevice v1 carried in the ordinary FujiBus packet. The full read response
 * is capacity bytes: 6-byte header, status byte, 10-byte file header, data. */
bool probe_bulk_start(struct probe_bulk *b, uint16_t capacity, const char *uri) {
    size_t len = strlen(uri);
    if (!len || len >= sizeof b->uri || capacity <= 17 ||
        capacity <= 15 + len || capacity > PL_MAX_BODY) return false;
    memset(b, 0, sizeof *b);
    memcpy(b->uri, uri, len + 1);
    b->capacity = capacity;
    b->size = capacity - 17;
    b->stage = BULK_STAT;
    return true;
}

static uint8_t bulk_pattern(size_t i) {
    return (uint8_t)((i * 73u + (i >> 3) + 19u) & 255u);
}

size_t probe_bulk_request(struct probe_bulk *b, uint8_t *out, size_t limit) {
    size_t uri = strlen(b->uri), n = 9 + uri;
    if (b->stage != BULK_STAT && b->stage != BULK_WRITE && b->stage != BULK_READ) return 0;
    if (b->stage == BULK_WRITE || b->stage == BULK_READ) n += 6;
    if (b->stage == BULK_WRITE) {
        size_t room = b->capacity - n;
        size_t remaining = b->size - b->offset;
        b->chunk = (uint16_t)(remaining < room ? remaining : room);
        n += b->chunk;
    } else if (b->stage == BULK_READ) b->chunk = b->size;
    if (n > limit || n > b->capacity) return 0;
    memset(out, 0, n);
    out[0] = 0xfe;
    out[1] = b->stage == BULK_STAT ? 1 : b->stage == BULK_WRITE ? 4 : 3;
    out[2] = (uint8_t)n; out[3] = (uint8_t)(n >> 8);
    out[6] = 1; out[7] = (uint8_t)uri; out[8] = (uint8_t)(uri >> 8);
    memcpy(out + 9, b->uri, uri);
    if (b->stage != BULK_STAT) {
        size_t at = 9 + uri;
        uint16_t off = b->stage == BULK_READ ? 0 : b->offset;
        out[at] = (uint8_t)off; out[at + 1] = (uint8_t)(off >> 8);
        out[at + 4] = (uint8_t)b->chunk; out[at + 5] = (uint8_t)(b->chunk >> 8);
        if (b->stage == BULK_WRITE)
            for (size_t i = 0; i < b->chunk; ++i) out[at + 6 + i] = bulk_pattern(off + i);
    }
    unsigned sum = 0;
    for (size_t i = 0; i < n; ++i) { sum += out[i]; sum = (sum & 255u) + (sum >> 8); }
    out[4] = (uint8_t)sum;
    return n;
}

bool probe_bulk_response(struct probe_bulk *b, const uint8_t *p, size_t n) {
    uint8_t command = b->stage == BULK_STAT ? 1 : b->stage == BULK_WRITE ? 4 : 3;
    if (!probe_raw_valid(p, n) || n < 17 || p[0] != 0xfe || p[1] != command ||
        p[5] != 1 || p[6] != 0 || p[7] != 1 || p[9] || p[10]) goto fail;
    if (b->stage == BULK_STAT) {
        if (n != 27 || (p[8] & 2)) goto fail; /* Never overwrite an existing file. */
        b->stage = BULK_WRITE;
    } else if (b->stage == BULK_WRITE) {
        if (n != 17 || p[8] || p[11] != (uint8_t)b->offset ||
            p[12] != (uint8_t)(b->offset >> 8) || p[13] || p[14] ||
            (p[15] | (unsigned)p[16] << 8) != b->chunk) goto fail;
        b->offset += b->chunk;
        if (b->offset == b->size) b->stage = BULK_READ;
    } else if (b->stage == BULK_READ) {
        if (n != (size_t)17 + b->size || p[11] || p[12] || p[13] || p[14] ||
            (p[15] | (unsigned)p[16] << 8) != b->size) goto fail;
        for (size_t i = 0; i < b->size; ++i)
            if (p[17 + i] != bulk_pattern(i)) goto fail;
        b->stage = BULK_DONE;
    } else goto fail;
    return true;
fail:
    b->stage = BULK_FAILED;
    return false;
}
