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
