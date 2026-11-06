#include "probe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

int main(void) {
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
