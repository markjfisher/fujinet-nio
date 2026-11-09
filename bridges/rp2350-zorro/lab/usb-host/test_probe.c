#include "probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void checksum(uint8_t *p, size_t n) {
    p[4] = 0;
    unsigned sum = 0;
    for (size_t i = 0; i < n; ++i) { sum += p[i]; sum = (sum & 255u) + (sum >> 8); }
    p[4] = (uint8_t)sum;
}

static void bulk_test(void) {
    struct probe_bulk b;
    uint8_t packet[PL_MAX_BODY], response[PL_MAX_BODY], data[PL_MAX_BODY];
    const char *uri = "flash:/zorro-usb-bench-test.bin";
    CHECK(!probe_bulk_start(&b, 17, uri));
    CHECK(probe_bulk_start(&b, 4096, uri));
    CHECK(b.stage == BULK_STAT && b.size == 4079);
    size_t n = probe_bulk_request(&b, packet, sizeof packet);
    CHECK(n == 9 + strlen(uri) && probe_raw_valid(packet, n));
    /* Independent fixture checksums from py/fujinet_tools/fujibus.py. */
    CHECK(packet[0] == 0xfe && packet[1] == 1 && packet[6] == 1 && packet[4] == 0xfe);
    memset(response, 0, 27);
    response[0] = 0xfe; response[1] = 1; response[2] = 27;
    response[5] = 1; response[7] = 1;
    response[8] = 2; checksum(response, 27);
    CHECK(!probe_bulk_response(&b, response, 27)); /* existing path */
    CHECK(b.stage == BULK_FAILED);
    CHECK(probe_bulk_start(&b, 4096, uri));
    response[8] = 0; checksum(response, 27);
    CHECK(probe_bulk_response(&b, response, 27));
    CHECK(b.stage == BULK_WRITE);
    n = probe_bulk_request(&b, packet, sizeof packet);
    CHECK(n == 4096 && probe_raw_valid(packet, n) && packet[4] == 0x27);
    size_t first = b.chunk;
    CHECK(first < b.size && first > 512);
    memcpy(data, packet + n - first, first);
    memset(response, 0, 17);
    response[0] = 0xfe; response[1] = 4; response[2] = 17;
    response[5] = 1; response[7] = 1;
    response[15] = (uint8_t)first; response[16] = (uint8_t)(first >> 8);
    checksum(response, 17);
    CHECK(probe_bulk_response(&b, response, 17));
    n = probe_bulk_request(&b, packet, sizeof packet);
    CHECK(probe_raw_valid(packet, n) && b.offset == first);
    memcpy(data + first, packet + n - b.chunk, b.chunk);
    response[11] = (uint8_t)first; response[12] = (uint8_t)(first >> 8);
    response[15] = (uint8_t)b.chunk; response[16] = (uint8_t)(b.chunk >> 8);
    checksum(response, 17);
    CHECK(probe_bulk_response(&b, response, 17));
    CHECK(b.stage == BULK_READ);
    n = probe_bulk_request(&b, packet, sizeof packet);
    CHECK(probe_raw_valid(packet, n) && packet[1] == 3 && packet[4] == 0x06);
    memset(response, 0, 4096);
    response[0] = 0xfe; response[1] = 3; response[3] = 16;
    response[5] = 1; response[7] = 1; response[8] = 2;
    response[15] = (uint8_t)b.size; response[16] = (uint8_t)(b.size >> 8);
    memcpy(response + 17, data, b.size); checksum(response, 4096);
    struct probe_bulk copy = b;
    response[123] ^= 1; checksum(response, 4096);
    CHECK(!probe_bulk_response(&copy, response, 4096)); /* valid checksum, wrong byte */
    response[123] ^= 1; checksum(response, 4096);
    CHECK(probe_bulk_response(&b, response, 4096) && b.stage == BULK_DONE);
    CHECK(!probe_bulk_response(&b, response, 4096));
}

int main(void) {
    bulk_test();
    struct probe_queue q = {0};
    uint8_t pattern[PL_MAX_BODY + PL_HEADER + PL_CRC];
    for (size_t i = 0; i < sizeof pattern; ++i) pattern[i] = (uint8_t)i;
    /* Complete maximum records survive arbitrary short writes and wraparound. */
    for (unsigned round = 0; round < 5; ++round) {
        CHECK(probe_append(&q, pattern, sizeof pattern));
        CHECK(probe_append(&q, pattern, sizeof pattern));
        CHECK(!probe_append(&q, pattern, 1));
        size_t offset = 0;
        while (q.used) {
            const uint8_t *p;
            size_t n = probe_front(&q, &p);
            if (n > 37) n = 37;
            for (size_t i = 0; i < n; ++i)
                CHECK(p[i] == pattern[(offset + i) % sizeof pattern]);
            CHECK(probe_consume(&q, 0)); /* USB temporarily accepts nothing. */
            CHECK(probe_consume(&q, n));
            offset += n;
        }
        CHECK(!probe_consume(&q, 1));
        /* Move ring origin so subsequent records cross the array boundary. */
        CHECK(probe_append(&q, pattern, 13));
        CHECK(probe_consume(&q, 13));
    }
    CHECK(probe_raw_valid(probe_clock_request, 6));
    /* Independent literal canonical response: time 0x12345678. */
    uint8_t reply[] = {0x45,1,19,0,0x70,1,0,1,0,0,0,0x78,0x56,0x34,0x12,0,0,0,0};
    uint8_t status;
    uint64_t seconds;
    CHECK(probe_clock(reply, sizeof reply, &status, &seconds));
    CHECK(status == 0 && seconds == 0x12345678);
    CHECK(!probe_clock(reply, sizeof reply - 1, &status, &seconds));
    reply[11] ^= 1;
    CHECK(!probe_clock(reply, sizeof reply, &status, &seconds));
    uint8_t error[] = {0x45,1,7,0,0x4f,1,1};
    CHECK(probe_clock(error, sizeof error, &status, &seconds));
    CHECK(status == 1 && seconds == 0);
    puts("USB probe queue and raw response tests passed");
}
