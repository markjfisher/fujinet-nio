#include "link_protocol.h"

#include <string.h>

uint16_t link_test_checksum(const uint8_t *data, size_t length) {
    /* A small CRC-CCITT implementation keeps the fixed test slot self-checking
       without introducing a product protocol dependency. */
    uint16_t crc = 0xffffu;
    size_t i;
    for (i = 0; i < length; ++i) {
        unsigned bit;
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    return crc;
}

void link_test_fill_payload(uint8_t *data, size_t length,
                            enum link_test_pattern pattern, uint32_t seed) {
    /* Each pattern is deterministic so a host test, RP2350 and ESP32 can all
       reproduce the exact byte sequence without exchanging test metadata. */
    size_t i;
    for (i = 0; i < length; ++i) {
        switch (pattern) {
        case LINK_PATTERN_ZERO:
            data[i] = 0;
            break;
        case LINK_PATTERN_FF:
            data[i] = 0xff;
            break;
        case LINK_PATTERN_INCREMENT:
            data[i] = (uint8_t)(seed + i);
            break;
        case LINK_PATTERN_ALTERNATING:
            data[i] = (i & 1u) ? 0x55 : 0xaa;
            break;
        case LINK_PATTERN_FIXED_RANDOM:
            seed = seed * 1664525u + 1013904223u;
            data[i] = (uint8_t)(seed >> 24);
            break;
        case LINK_PATTERN_SLIP_BYTES:
            data[i] = (uint8_t)((const uint8_t[]){0x00, 0xff, 0xaa, 0x55, 0xc0, 0xdb}[i % 6u]);
            break;
        }
    }
}

void link_test_make_frame(struct link_test_frame *frame, uint8_t scenario,
                          uint32_t sequence, size_t length,
                          enum link_test_pattern pattern) {
    /* Fill the entire fixed-size slot so trailing bytes cannot accidentally
       preserve data from an earlier experiment. */
    memset(frame, 0, sizeof(*frame));
    frame->magic = LINK_TEST_MAGIC;
    frame->version = LINK_TEST_VERSION;
    frame->scenario = scenario;
    frame->sequence = sequence;
    if (length > LINK_TEST_MAX_PAYLOAD) {
        frame->status = LINK_STATUS_BAD_LENGTH;
        return;
    }
    frame->payload_length = (uint16_t)length;
    link_test_fill_payload(frame->payload, length, pattern, sequence ^ 0x6d2b79f5u);
    frame->checksum = link_test_checksum(frame->payload, length);
}

enum link_test_status link_test_validate_frame(const struct link_test_frame *frame) {
    /* Validate only the framing contract used by this lab.  The payload itself
       remains opaque to the link test. */
    if (frame->magic != LINK_TEST_MAGIC)
        return LINK_STATUS_BAD_MAGIC;
    if (frame->version != LINK_TEST_VERSION)
        return LINK_STATUS_BAD_VERSION;
    if (frame->payload_length > LINK_TEST_MAX_PAYLOAD)
        return LINK_STATUS_BAD_LENGTH;
    if (link_test_checksum(frame->payload, frame->payload_length) != frame->checksum)
        return LINK_STATUS_BAD_CHECKSUM;
    return LINK_STATUS_OK;
}

void link_test_make_echo(const struct link_test_frame *request,
                         struct link_test_frame *response,
                         enum link_test_status status) {
    /* The ESP returns a fresh slot.  For an invalid request it reports only the
       framing status; a valid request is echoed byte-for-byte. */
    memset(response, 0, sizeof(*response));
    response->magic = LINK_TEST_MAGIC;
    response->version = LINK_TEST_VERSION;
    response->scenario = request->scenario;
    response->status = (uint8_t)status;
    response->sequence = request->sequence;
    if (status != LINK_STATUS_OK) return;
    response->payload_length = request->payload_length;
    memcpy(response->payload, request->payload, request->payload_length);
    response->checksum = link_test_checksum(response->payload, response->payload_length);
}

const char *link_test_status_name(enum link_test_status status) {
    static const char *const names[] = {
        "ok",
        "bad_magic",
        "bad_version",
        "bad_length",
        "bad_checksum",
        "unsupported",
    };
    return status <= LINK_STATUS_UNSUPPORTED ? names[status] : "unknown";
}
