#include "link_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_patterns(void) {
    /* The runner and both endpoints independently generate these vectors.  They
       must therefore be deterministic, including troublesome framing bytes. */
    uint8_t a[12], b[12];
    link_test_fill_payload(a, sizeof(a), LINK_PATTERN_SLIP_BYTES, 0);
    assert(a[0] == 0 && a[4] == 0xc0 && a[5] == 0xdb);
    link_test_fill_payload(a, sizeof(a), LINK_PATTERN_FIXED_RANDOM, 99);
    link_test_fill_payload(b, sizeof(b), LINK_PATTERN_FIXED_RANDOM, 99);
    assert(memcmp(a, b, sizeof(a)) == 0);
}

static void test_frame_and_echo(void) {
    /* A valid request must remain valid after the ESP's byte-for-byte echo; a
       one-bit payload corruption must be caught by the framing CRC. */
    struct link_test_frame request, response;
    link_test_make_frame(&request, 1, 7, 17, LINK_PATTERN_INCREMENT);
    assert(link_test_validate_frame(&request) == LINK_STATUS_OK);
    link_test_make_echo(&request, &response, LINK_STATUS_OK);
    assert(response.status == LINK_STATUS_OK);
    assert(link_test_validate_frame(&response) == LINK_STATUS_OK);
    assert(response.sequence == 7 && response.payload_length == 17);
    assert(memcmp(response.payload, request.payload, 17) == 0);
    request.payload[5] ^= 1;
    assert(link_test_validate_frame(&request) == LINK_STATUS_BAD_CHECKSUM);
}

static void test_bounds(void) {
    /* The fixed 256-byte slot must reject an oversized payload before either
       endpoint attempts an SPI transfer. */
    struct link_test_frame frame;
    link_test_make_frame(&frame, 2, 8, LINK_TEST_MAX_PAYLOAD, LINK_PATTERN_FF);
    assert(link_test_validate_frame(&frame) == LINK_STATUS_OK);
    link_test_make_frame(&frame, 2, 8, LINK_TEST_MAX_PAYLOAD + 1u, LINK_PATTERN_FF);
    assert(frame.status == LINK_STATUS_BAD_LENGTH);
}

int main(void) {
    test_patterns();
    test_frame_and_echo();
    test_bounds();
    puts("link protocol tests passed");
    return 0;
}
