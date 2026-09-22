#ifndef RP2350_ZORRO_LINK_PROTOCOL_H
#define RP2350_ZORRO_LINK_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Feasibility-only SPI slot format.  It is deliberately not a FujiBus ABI. */
#define LINK_TEST_MAGIC 0x4c4e4b31u /* "LNK1" */
#define LINK_TEST_VERSION 1u
#define LINK_TEST_MAX_PAYLOAD 240u
/* Feasibility transport flag: suppress peer USB logs during timed batches. */
#define LINK_TEST_FLAG_QUIET 0x80u

enum link_test_pattern {
    LINK_PATTERN_ZERO = 0,
    LINK_PATTERN_FF,
    LINK_PATTERN_INCREMENT,
    LINK_PATTERN_ALTERNATING,
    LINK_PATTERN_FIXED_RANDOM,
    LINK_PATTERN_SLIP_BYTES,
};

enum link_test_status {
    LINK_STATUS_OK = 0,
    LINK_STATUS_BAD_MAGIC = 1,
    LINK_STATUS_BAD_VERSION = 2,
    LINK_STATUS_BAD_LENGTH = 3,
    LINK_STATUS_BAD_CHECKSUM = 4,
    LINK_STATUS_UNSUPPORTED = 5,
};

struct link_test_frame {
    uint32_t magic;
    uint8_t version;
    uint8_t scenario;
    uint8_t status;
    uint8_t reserved;
    uint32_t sequence;
    uint16_t payload_length;
    uint16_t checksum;
    uint8_t payload[LINK_TEST_MAX_PAYLOAD];
};

uint16_t link_test_checksum(const uint8_t *data, size_t length);
void link_test_fill_payload(uint8_t *data, size_t length,
                            enum link_test_pattern pattern, uint32_t seed);
void link_test_make_frame(struct link_test_frame *frame, uint8_t scenario,
                          uint32_t sequence, size_t length,
                          enum link_test_pattern pattern);
enum link_test_status link_test_validate_frame(const struct link_test_frame *frame);
void link_test_make_echo(const struct link_test_frame *request,
                         struct link_test_frame *response,
                         enum link_test_status status);
const char *link_test_status_name(enum link_test_status status);

#endif
