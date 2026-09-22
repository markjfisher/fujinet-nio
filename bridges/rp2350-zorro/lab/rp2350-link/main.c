#include "link_protocol.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LINK_DEFAULT_SCENARIO
#define LINK_DEFAULT_SCENARIO 0
#endif
#ifndef LINK_SPI_BAUD_HZ
#define LINK_SPI_BAUD_HZ 1000000u
#endif

/* Provisional W2 lab mapping; replace through CMake cache values for another bench. */
#ifndef LINK_RP_SCK_PIN
#define LINK_RP_SCK_PIN 2
#define LINK_RP_MOSI_PIN 3
#define LINK_RP_MISO_PIN 4
#define LINK_RP_CS_PIN 5
#define LINK_RP_READY_PIN 6
#define LINK_RP_DATA_AVAILABLE_PIN 7
#endif

/* Fixed-size buffers make the two SPI slots explicit: request, ignored first
   response, and echoed response.  zero_frame is also used to drain stale work. */
static struct link_test_frame request_frame;
static struct link_test_frame discard_frame;
static struct link_test_frame response_frame;
static struct link_test_frame zero_frame;

static bool wait_for(uint pin, bool value, uint32_t timeout_ms) {
    /* Flow-control lines are level signals.  Keep their waits bounded so a
       missing wire or reset peer produces a machine-readable result. */
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (gpio_get(pin) != value) {
        if (time_reached(deadline)) return false;
        tight_loop_contents();
    }
    return true;
}

static void transaction(const struct link_test_frame *tx, struct link_test_frame *rx) {
    /* One lab transaction always clocks exactly one complete fixed-size slot. */
    gpio_put(LINK_RP_CS_PIN, 0);
    spi_write_read_blocking(spi0, (const uint8_t *)tx, (uint8_t *)rx, sizeof(*tx));
    gpio_put(LINK_RP_CS_PIN, 1);
}

static void transaction_bytes(const struct link_test_frame *tx,
                              struct link_test_frame *rx,
                              size_t byte_count) {
    /* L2 intentionally releases CS before a complete slot has been clocked. */
    gpio_put(LINK_RP_CS_PIN, 0);
    spi_write_read_blocking(spi0, (const uint8_t *)tx, (uint8_t *)rx, byte_count);
    gpio_put(LINK_RP_CS_PIN, 1);
}

/* The ESP endpoint is intentionally persistent between lab runs.  A stopped
   or failed prior run can leave its prepared response advertised.  Consume it
   before starting a new request so DATA_AVAILABLE always describes this run. */
static bool drain_stale_response(void) {
    if (!gpio_get(LINK_RP_DATA_AVAILABLE_PIN)) return true;
    if (!wait_for(LINK_RP_READY_PIN, true, 1000)) return false;
    transaction(&zero_frame, &discard_frame);
    return wait_for(LINK_RP_DATA_AVAILABLE_PIN, false, 1000);
}

static void run_once(unsigned scenario, uint32_t sequence, size_t length,
                     enum link_test_pattern pattern, uint8_t control) {
    enum link_test_status status;

    /* Establish a known mailbox state before each independent runner case. */
    if (!drain_stale_response()) {
        puts("result protocol=link-feasibility-v1 status=timeout_draining_stale_response");
        return;
    }
    link_test_make_frame(&request_frame, (uint8_t)scenario, sequence, length, pattern);
    request_frame.reserved = control;
    if (request_frame.status != LINK_STATUS_OK) {
        printf("result protocol=link-feasibility-v1 status=local_%s scenario=L%u length=%u\n",
               link_test_status_name((enum link_test_status)request_frame.status), scenario, (unsigned)length);
        return;
    }
    if (!wait_for(LINK_RP_READY_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=timeout_waiting_ready");
        return;
    }
    /* The slave validates this request while it shifts out an irrelevant slot. */
    transaction(&request_frame, &discard_frame);
    if (!wait_for(LINK_RP_DATA_AVAILABLE_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=timeout_waiting_response");
        return;
    }
    /* DATA_AVAILABLE now refers to this request's prepared echo. */
    transaction(&zero_frame, &response_frame);
    status = link_test_validate_frame(&response_frame);
    if (status != LINK_STATUS_OK || response_frame.status != LINK_STATUS_OK ||
        response_frame.sequence != request_frame.sequence ||
        response_frame.payload_length != request_frame.payload_length ||
        memcmp(response_frame.payload, request_frame.payload, request_frame.payload_length) != 0) {
        printf("result protocol=link-feasibility-v1 status=echo_mismatch scenario=L%u frame=%s peer=%s sequence=%lu length=%u\n",
               scenario, link_test_status_name(status),
               link_test_status_name((enum link_test_status)response_frame.status),
               (unsigned long)response_frame.sequence, response_frame.payload_length);
        return;
    }
    printf("result protocol=link-feasibility-v1 status=passed scenario=L%u sequence=%lu length=%u checksum=%04x\n",
           scenario, (unsigned long)response_frame.sequence, response_frame.payload_length,
           response_frame.checksum);
}

static bool frame_matches(const struct link_test_frame *actual,
                          const struct link_test_frame *expected) {
    return link_test_validate_frame(actual) == LINK_STATUS_OK &&
           actual->status == LINK_STATUS_OK &&
           actual->sequence == expected->sequence &&
           actual->payload_length == expected->payload_length &&
           memcmp(actual->payload, expected->payload, expected->payload_length) == 0;
}

static void run_scheduled(unsigned scenario, uint32_t outgoing_sequence,
                          uint32_t request_sequence, size_t request_length,
                          enum link_test_pattern request_pattern) {
    static const size_t outgoing_lengths[] = {1, 64, 240};
    struct link_test_frame expected_outgoing;
    enum link_test_pattern outgoing_pattern;

    /* L5 deliberately uses SPI full-duplex: the first request clock both sends
       RP work and receives an ESP-originated frame.  The following zero slot
       drains only the request's echo. */
    outgoing_pattern = (enum link_test_pattern)((outgoing_sequence - 1u) % 6u);
    link_test_make_frame(&expected_outgoing, (uint8_t)scenario, outgoing_sequence,
                         outgoing_lengths[(outgoing_sequence - 1u) % 3u],
                         outgoing_pattern);
    link_test_make_frame(&request_frame, (uint8_t)scenario, request_sequence,
                         request_length, request_pattern);
    if (request_frame.status != LINK_STATUS_OK ||
        !wait_for(LINK_RP_READY_PIN, true, 1000) ||
        !wait_for(LINK_RP_DATA_AVAILABLE_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=schedule_waiting_peer");
        return;
    }
    transaction(&request_frame, &response_frame);
    if (!frame_matches(&response_frame, &expected_outgoing)) {
        puts("result protocol=link-feasibility-v1 status=schedule_outgoing_mismatch");
        return;
    }
    if (!wait_for(LINK_RP_DATA_AVAILABLE_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=schedule_waiting_echo");
        return;
    }
    transaction(&zero_frame, &response_frame);
    if (!frame_matches(&response_frame, &request_frame)) {
        puts("result protocol=link-feasibility-v1 status=schedule_echo_mismatch");
        return;
    }
    printf("result protocol=link-feasibility-v1 status=scheduled scenario=L%u "
           "outgoing_sequence=%lu request_sequence=%lu length=%u\n", scenario,
           (unsigned long)outgoing_sequence, (unsigned long)request_sequence,
           (unsigned)request_length);
}

static void run_receive(unsigned scenario, uint32_t sequence) {
    static const size_t lengths[] = {1, 64, 240};
    struct link_test_frame expected;
    enum link_test_status status;
    enum link_test_pattern pattern = (enum link_test_pattern)((sequence - 1u) % 6u);

    /* L4 consumes a frame scheduled by the ESP at boot; no RP request creates it. */
    if (!wait_for(LINK_RP_DATA_AVAILABLE_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=timeout_waiting_independent_data");
        return;
    }
    transaction(&zero_frame, &response_frame);
    link_test_make_frame(&expected, (uint8_t)scenario, sequence,
                         lengths[(sequence - 1u) % 3u], pattern);
    status = link_test_validate_frame(&response_frame);
    if (status != LINK_STATUS_OK || response_frame.status != LINK_STATUS_OK ||
        response_frame.sequence != expected.sequence ||
        response_frame.payload_length != expected.payload_length ||
        memcmp(response_frame.payload, expected.payload, expected.payload_length) != 0) {
        printf("result protocol=link-feasibility-v1 status=independent_mismatch "
               "scenario=L%u frame=%s sequence=%lu length=%u\n", scenario,
               link_test_status_name(status), (unsigned long)response_frame.sequence,
               response_frame.payload_length);
        return;
    }
    printf("result protocol=link-feasibility-v1 status=received scenario=L%u "
           "sequence=%lu length=%u checksum=%04x\n", scenario,
           (unsigned long)response_frame.sequence, response_frame.payload_length,
           response_frame.checksum);
}

static void run_oversize(unsigned scenario, uint32_t sequence, size_t length,
                         enum link_test_pattern pattern) {
    /* The frame contract rejects payloads larger than its fixed slot before
       they can become a malformed physical transfer. */
    link_test_make_frame(&request_frame, (uint8_t)scenario, sequence, length, pattern);
    if (request_frame.status == LINK_STATUS_BAD_LENGTH) {
        printf("result protocol=link-feasibility-v1 status=oversize_rejected "
               "scenario=L%u sequence=%lu length=%u\n",
               scenario, (unsigned long)sequence, (unsigned)length);
        return;
    }
    puts("result protocol=link-feasibility-v1 status=oversize_unexpected_accept");
}

static void run_partial(unsigned scenario, uint32_t sequence, size_t length,
                        enum link_test_pattern pattern, size_t slot_bytes) {
    enum link_test_status frame_status;

    if (slot_bytes == 0 || slot_bytes >= sizeof(request_frame) ||
        !drain_stale_response()) {
        puts("result protocol=link-feasibility-v1 status=partial_setup_failed");
        return;
    }
    link_test_make_frame(&request_frame, (uint8_t)scenario, sequence, length, pattern);
    if (request_frame.status != LINK_STATUS_OK ||
        !wait_for(LINK_RP_READY_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=partial_waiting_ready");
        return;
    }

    memset(&discard_frame, 0, sizeof(discard_frame));
    transaction_bytes(&request_frame, &discard_frame, slot_bytes);
    if (!wait_for(LINK_RP_DATA_AVAILABLE_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=partial_timeout_waiting_response");
        return;
    }
    transaction(&zero_frame, &response_frame);
    frame_status = link_test_validate_frame(&response_frame);
    if (frame_status == LINK_STATUS_OK &&
        response_frame.status == LINK_STATUS_BAD_CHECKSUM) {
        printf("result protocol=link-feasibility-v1 status=partial_rejected "
               "scenario=L%u sequence=%lu slot_bytes=%u peer=bad_checksum\n",
               scenario, (unsigned long)sequence, (unsigned)slot_bytes);
        return;
    }
    printf("result protocol=link-feasibility-v1 status=partial_unexpected_response "
           "frame=%s peer=%s\n",
           link_test_status_name(frame_status),
           link_test_status_name((enum link_test_status)response_frame.status));
}

int main(void) {
    char line[64];

    /* Configure the physical SPI master and two ESP-to-RP flow-control inputs. */
    stdio_init_all();
    spi_init(spi0, LINK_SPI_BAUD_HZ);
    gpio_set_function(LINK_RP_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LINK_RP_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LINK_RP_MISO_PIN, GPIO_FUNC_SPI);
    gpio_init(LINK_RP_CS_PIN);
    gpio_set_dir(LINK_RP_CS_PIN, GPIO_OUT);
    gpio_put(LINK_RP_CS_PIN, 1);

    gpio_init(LINK_RP_READY_PIN);
    gpio_set_dir(LINK_RP_READY_PIN, GPIO_IN);
    gpio_pull_down(LINK_RP_READY_PIN);

    gpio_init(LINK_RP_DATA_AVAILABLE_PIN);
    gpio_set_dir(LINK_RP_DATA_AVAILABLE_PIN, GPIO_IN);
    gpio_pull_down(LINK_RP_DATA_AVAILABLE_PIN);
    sleep_ms(500);
    printf("ready protocol=link-feasibility-v1 default=L%d spi_hz=%u slot_bytes=%u\n",
           LINK_DEFAULT_SCENARIO, LINK_SPI_BAUD_HZ, (unsigned)sizeof(request_frame));
    for (;;) {
        /* USB CDC supplies one simple command per runner case. */
        if (fgets(line, sizeof(line), stdin) == NULL) {
            sleep_ms(10);
            continue;
        }
        if (strncmp(line, "run", 3) == 0) {
            unsigned scenario = LINK_DEFAULT_SCENARIO;
            unsigned length = 16;
            unsigned pattern = LINK_PATTERN_INCREMENT;
            unsigned sequence = 1;
            (void)sscanf(line + 3, "%u %u %u %u", &scenario, &length, &pattern, &sequence);
            run_once(scenario, sequence, length, (enum link_test_pattern)pattern, 0);
        } else if (strncmp(line, "pressure", 8) == 0) {
            unsigned scenario = 3;
            unsigned length = 64;
            unsigned pattern = LINK_PATTERN_INCREMENT;
            unsigned sequence = 1;
            unsigned queue_depth = 1;
            unsigned pause_ms = 0;
            (void)sscanf(line + 8, "%u %u %u %u %u %u", &scenario, &length,
                         &pattern, &sequence, &queue_depth, &pause_ms);
            /* L3 stores a bounded queue depth and pause selector in the
               feasibility-only reserved byte. */
            unsigned pause_code = pause_ms == 0 ? 0 : pause_ms == 1 ? 1 :
                                  pause_ms == 10 ? 2 : pause_ms == 100 ? 3 : 255;
            if (queue_depth < 1 || queue_depth > 4 || pause_code == 255) {
                puts("result protocol=link-feasibility-v1 status=pressure_invalid_configuration");
            } else {
                run_once(scenario, sequence, length, (enum link_test_pattern)pattern,
                         (uint8_t)(queue_depth | (pause_code << 4)));
            }
        } else if (strncmp(line, "schedule", 8) == 0) {
            unsigned scenario = 5;
            unsigned outgoing_sequence = 1;
            unsigned request_sequence = 101;
            unsigned length = 64;
            unsigned pattern = LINK_PATTERN_INCREMENT;
            (void)sscanf(line + 8, "%u %u %u %u %u", &scenario,
                         &outgoing_sequence, &request_sequence, &length, &pattern);
            run_scheduled(scenario, outgoing_sequence, request_sequence, length,
                          (enum link_test_pattern)pattern);
        } else if (strncmp(line, "receive", 7) == 0) {
            unsigned scenario = LINK_DEFAULT_SCENARIO;
            unsigned sequence = 1;
            (void)sscanf(line + 7, "%u %u", &scenario, &sequence);
            run_receive(scenario, sequence);
        } else if (strncmp(line, "oversize", 8) == 0) {
            unsigned scenario = LINK_DEFAULT_SCENARIO;
            unsigned length = LINK_TEST_MAX_PAYLOAD + 1u;
            unsigned pattern = LINK_PATTERN_ZERO;
            unsigned sequence = 1;
            (void)sscanf(line + 8, "%u %u %u %u", &scenario, &length, &pattern, &sequence);
            run_oversize(scenario, sequence, length, (enum link_test_pattern)pattern);
        } else if (strncmp(line, "partial", 7) == 0) {
            unsigned scenario = LINK_DEFAULT_SCENARIO;
            unsigned length = 64;
            unsigned pattern = LINK_PATTERN_INCREMENT;
            unsigned sequence = 1;
            unsigned slot_bytes = 32;
            (void)sscanf(line + 7, "%u %u %u %u %u", &scenario, &length,
                         &pattern, &sequence, &slot_bytes);
            run_partial(scenario, sequence, length, (enum link_test_pattern)pattern,
                        slot_bytes);
        } else if (strncmp(line, "pins", 4) == 0) {
            printf("pins sck=%d mosi=%d miso=%d cs=%d ready=%d data_available=%d\n", LINK_RP_SCK_PIN, LINK_RP_MOSI_PIN, LINK_RP_MISO_PIN, LINK_RP_CS_PIN, LINK_RP_READY_PIN, LINK_RP_DATA_AVAILABLE_PIN);
        } else if (strncmp(line, "help", 4) == 0) {
            puts("commands: run/oversize [scenario length pattern sequence], "
                 "receive [scenario sequence], "
                 "schedule [scenario outgoing_sequence request_sequence length pattern], "
                 "pressure [scenario length pattern sequence queue_depth pause_ms], "
                 "partial [scenario length pattern sequence slot_bytes], pins, help");
        } else {
            puts("error protocol=link-feasibility-v1 command");
        }
    }
}
