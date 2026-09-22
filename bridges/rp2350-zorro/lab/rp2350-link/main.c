#include "link_protocol.h"

#include "hardware/dma.h"
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
/* Batch/soak summaries expose the first transport step that failed without
   printing every successful transaction. */
static const char *exchange_failure = "not_started";

enum link_datapath {
    LINK_DATAPATH_POLLING,
    LINK_DATAPATH_DMA,
};

/* These phase timings measure the RP2350 master side of the existing lab
   envelope. They make the cost of ownership waits visible without claiming
   that the test-only fixed slot or READY interval is a production contract. */
struct exchange_timing {
    uint64_t wait_ready_us;
    uint64_t request_transfer_us;
    uint64_t wait_response_us;
    uint64_t response_transfer_us;
    uint64_t rearm_us;
};

struct timing_totals {
    uint64_t wait_ready_us;
    uint64_t request_transfer_us;
    uint64_t wait_response_us;
    uint64_t response_transfer_us;
    uint64_t rearm_us;
};

static int dma_tx_channel = -1;
static int dma_rx_channel = -1;

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

static void transaction_dma(const struct link_test_frame *tx,
                            struct link_test_frame *rx) {
    dma_channel_config rx_config;
    dma_channel_config tx_config;

    /* The SPI peripheral remains the wire owner. DMA only moves one complete
       fixed slot to and from its FIFOs while ARM retains CS and GPIO ownership. */
    rx_config = dma_channel_get_default_config((uint)dma_rx_channel);
    channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_8);
    channel_config_set_read_increment(&rx_config, false);
    channel_config_set_write_increment(&rx_config, true);
    channel_config_set_dreq(&rx_config, spi_get_dreq(spi0, false));
    tx_config = dma_channel_get_default_config((uint)dma_tx_channel);
    channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_8);
    channel_config_set_read_increment(&tx_config, true);
    channel_config_set_write_increment(&tx_config, false);
    channel_config_set_dreq(&tx_config, spi_get_dreq(spi0, true));

    /* Arm RX first so no received byte can be lost while TX starts the clock. */
    dma_channel_configure((uint)dma_rx_channel, &rx_config, rx,
                          &spi_get_hw(spi0)->dr, sizeof(*rx), false);
    dma_channel_configure((uint)dma_tx_channel, &tx_config, &spi_get_hw(spi0)->dr,
                          tx, sizeof(*tx), false);
    gpio_put(LINK_RP_CS_PIN, 0);
    dma_start_channel_mask((1u << (uint)dma_rx_channel) |
                           (1u << (uint)dma_tx_channel));
    dma_channel_wait_for_finish_blocking((uint)dma_tx_channel);
    dma_channel_wait_for_finish_blocking((uint)dma_rx_channel);
    gpio_put(LINK_RP_CS_PIN, 1);
}

static void transaction_for_datapath(enum link_datapath datapath,
                                     const struct link_test_frame *tx,
                                     struct link_test_frame *rx) {
    if (datapath == LINK_DATAPATH_DMA) {
        transaction_dma(tx, rx);
    } else {
        transaction(tx, rx);
    }
}

static bool wait_for_next_slot(void) {
    /* READY is a slot generation boundary. After a completed transaction the
       ESP drops it while it validates the received bytes and queues the next
       transmit slot. Waiting for low then high prevents a stale high level
       from being mistaken for readiness of the new slot. */
    return wait_for(LINK_RP_READY_PIN, false, 1000) &&
           wait_for(LINK_RP_READY_PIN, true, 1000);
}

/* The ESP endpoint is intentionally persistent between lab runs.  A stopped
   or failed prior run can leave its prepared response advertised.  Consume it
   before starting a new request so DATA_AVAILABLE always describes this run. */
static bool drain_stale_response(void) {
    if (!gpio_get(LINK_RP_DATA_AVAILABLE_PIN)) return true;
    if (!wait_for(LINK_RP_READY_PIN, true, 1000)) return false;
    transaction(&zero_frame, &discard_frame);
    /* Consuming a stale frame also retires a slot generation.  Wait through
       its READY low-to-high transition before a caller submits fresh work. */
    return wait_for_next_slot() && !gpio_get(LINK_RP_DATA_AVAILABLE_PIN);
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
        printf("result protocol=link-feasibility-v1 status=schedule_outgoing_mismatch "
               "frame=%s peer=%s sequence=%lu length=%u\n",
               link_test_status_name(link_test_validate_frame(&response_frame)),
               link_test_status_name((enum link_test_status)response_frame.status),
               (unsigned long)response_frame.sequence, response_frame.payload_length);
        return;
    }
    /* DATA_AVAILABLE was already high for the just-consumed ESP frame. Wait
       for READY's low-to-high re-arm boundary; only then does it describe the
       echo prepared from this RP request. */
    if (!wait_for_next_slot() || !wait_for(LINK_RP_DATA_AVAILABLE_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=schedule_waiting_echo");
        return;
    }
    transaction(&zero_frame, &response_frame);
    if (!frame_matches(&response_frame, &request_frame)) {
        printf("result protocol=link-feasibility-v1 status=schedule_echo_mismatch "
               "frame=%s peer=%s sequence=%lu length=%u\n",
               link_test_status_name(link_test_validate_frame(&response_frame)),
               link_test_status_name((enum link_test_status)response_frame.status),
               (unsigned long)response_frame.sequence, response_frame.payload_length);
        return;
    }
    /* Consuming the echo causes the ESP to queue the next autonomous frame.
       Do not let the following host command observe the old READY high level. */
    if (!wait_for_next_slot() || !wait_for(LINK_RP_DATA_AVAILABLE_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=schedule_waiting_next_outgoing");
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

static void run_fault_partial(unsigned scenario, uint32_t sequence, size_t length,
                              enum link_test_pattern pattern, size_t slot_bytes) {
    /* Leave the peer's explicit error response advertised. L6 resets the RP
       before it can consume it; the recovered image must discard it as stale. */
    if (slot_bytes == 0 || slot_bytes >= sizeof(request_frame) ||
        !drain_stale_response()) {
        puts("result protocol=link-feasibility-v1 status=fault_partial_setup_failed");
        return;
    }
    link_test_make_frame(&request_frame, (uint8_t)scenario, sequence, length, pattern);
    if (request_frame.status != LINK_STATUS_OK ||
        !wait_for(LINK_RP_READY_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=fault_partial_waiting_ready");
        return;
    }
    transaction_bytes(&request_frame, &discard_frame, slot_bytes);
    printf("result protocol=link-feasibility-v1 status=partial_injected "
           "scenario=L%u sequence=%lu slot_bytes=%u\n", scenario,
           (unsigned long)sequence, (unsigned)slot_bytes);
}

static void run_peer_reset(unsigned scenario, uint32_t sequence) {
    /* L7 requests a feasibility-only ESP reboot at a completed slot boundary.
       The RP then proves both flow-control outputs withdrew before recovery. */
    if (!drain_stale_response()) {
        puts("result protocol=link-feasibility-v1 status=peer_reset_setup_failed");
        return;
    }
    link_test_make_frame(&request_frame, (uint8_t)scenario, sequence, 1,
                         LINK_PATTERN_FIXED_RANDOM);
    request_frame.reserved = 0x7fu;
    if (!wait_for(LINK_RP_READY_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=peer_reset_waiting_ready");
        return;
    }
    transaction(&request_frame, &discard_frame);
    if (wait_for(LINK_RP_READY_PIN, false, 1000) &&
        wait_for(LINK_RP_DATA_AVAILABLE_PIN, false, 1000)) {
        printf("result protocol=link-feasibility-v1 status=peer_reset_detected "
               "scenario=L%u sequence=%lu\n", scenario, (unsigned long)sequence);
    } else {
        puts("result protocol=link-feasibility-v1 status=peer_reset_not_detected");
    }
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
    /* An error response intentionally has no payload, therefore no payload CRC
       to validate. Its framing and explicit peer status are the evidence. */
    if (response_frame.magic == LINK_TEST_MAGIC &&
        response_frame.version == LINK_TEST_VERSION &&
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

static bool exchange_quiet(unsigned scenario, uint32_t sequence, size_t length,
                           enum link_test_pattern pattern,
                           enum link_datapath datapath,
                           struct exchange_timing *timing) {
    uint64_t started_us;

    /* Batch and soak runs use the same two-slot exchange as run_once, but defer
       reporting until the measured group finishes so USB output cannot pace it. */
    memset(timing, 0, sizeof(*timing));
    exchange_failure = "stale_drain";
    if (!drain_stale_response()) return false;
    link_test_make_frame(&request_frame, (uint8_t)scenario, sequence, length, pattern);
    /* Batch timing measures the link, not ESP USB-console backpressure. */
    request_frame.reserved = LINK_TEST_FLAG_QUIET;
    exchange_failure = "wait_ready";
    started_us = time_us_64();
    if (request_frame.status != LINK_STATUS_OK ||
        !wait_for(LINK_RP_READY_PIN, true, 1000)) return false;
    timing->wait_ready_us = time_us_64() - started_us;
    started_us = time_us_64();
    transaction_for_datapath(datapath, &request_frame, &discard_frame);
    timing->request_transfer_us = time_us_64() - started_us;
    exchange_failure = "wait_response";
    started_us = time_us_64();
    if (!wait_for(LINK_RP_DATA_AVAILABLE_PIN, true, 1000)) return false;
    timing->wait_response_us = time_us_64() - started_us;
    started_us = time_us_64();
    transaction_for_datapath(datapath, &zero_frame, &response_frame);
    timing->response_transfer_us = time_us_64() - started_us;
    exchange_failure = "echo_mismatch";
    if (!frame_matches(&response_frame, &request_frame)) return false;
    /* spi_write_read_blocking() and DMA both return after the final bit is
       shifted. The ESP still needs to retire the echo and queue the next slot. */
    exchange_failure = "wait_rearm";
    started_us = time_us_64();
    if (!wait_for_next_slot() || gpio_get(LINK_RP_DATA_AVAILABLE_PIN)) return false;
    timing->rearm_us = time_us_64() - started_us;
    exchange_failure = "none";
    return true;
}

static void add_timing(struct timing_totals *totals,
                       const struct exchange_timing *timing) {
    totals->wait_ready_us += timing->wait_ready_us;
    totals->request_transfer_us += timing->request_transfer_us;
    totals->wait_response_us += timing->wait_response_us;
    totals->response_transfer_us += timing->response_transfer_us;
    totals->rearm_us += timing->rearm_us;
}

static const char *datapath_name(enum link_datapath datapath) {
    return datapath == LINK_DATAPATH_DMA ? "dma" : "polling";
}

static void run_batch(unsigned scenario, uint32_t first_sequence, unsigned count,
                      size_t length, enum link_test_pattern pattern,
                      uint32_t requested_hz, enum link_datapath datapath,
                      bool profile) {
    unsigned completed = 0;
    uint64_t start_us;
    uint64_t elapsed_us;
    uint32_t actual_hz;
    struct timing_totals totals = {0};
    struct exchange_timing timing;

    if (count == 0 || count > 1000 || requested_hz == 0) {
        puts("result protocol=link-feasibility-v1 status=batch_invalid_configuration");
        return;
    }
    actual_hz = spi_set_baudrate(spi0, requested_hz);
    start_us = time_us_64();
    while (completed < count &&
           exchange_quiet(scenario, first_sequence + completed, length, pattern,
                          datapath, &timing)) {
        add_timing(&totals, &timing);
        ++completed;
    }
    elapsed_us = time_us_64() - start_us;
    if (completed != count) {
        printf("result protocol=link-feasibility-v1 status=batch_failed datapath=%s completed=%u "
               "count=%u elapsed_us=%llu spi_hz=%lu reason=%s frame=%s peer=%s "
               "response_sequence=%lu response_length=%u\n", datapath_name(datapath),
               completed, count, (unsigned long long)elapsed_us, (unsigned long)actual_hz,
               exchange_failure, link_test_status_name(link_test_validate_frame(&response_frame)),
               link_test_status_name((enum link_test_status)response_frame.status),
               (unsigned long)response_frame.sequence, response_frame.payload_length);
        return;
    }
    if (!profile) {
        printf("result protocol=link-feasibility-v1 status=batch_pass count=%u length=%u "
               "payload_bytes=%lu elapsed_us=%llu spi_hz=%lu\n", count, (unsigned)length,
               (unsigned long)(count * length), (unsigned long long)elapsed_us,
               (unsigned long)actual_hz);
        return;
    }
    printf("result protocol=link-feasibility-v1 status=batch_pass datapath=%s count=%u length=%u "
           "payload_bytes=%lu elapsed_us=%llu spi_hz=%lu ready_wait_us_mean=%llu "
           "request_transfer_us_mean=%llu response_wait_us_mean=%llu "
           "response_transfer_us_mean=%llu rearm_us_mean=%llu\n", datapath_name(datapath),
           count, (unsigned)length, (unsigned long)(count * length),
           (unsigned long long)elapsed_us, (unsigned long)actual_hz,
           (unsigned long long)(totals.wait_ready_us / count),
           (unsigned long long)(totals.request_transfer_us / count),
           (unsigned long long)(totals.wait_response_us / count),
           (unsigned long long)(totals.response_transfer_us / count),
           (unsigned long long)(totals.rearm_us / count));
}

static void run_soak(unsigned scenario, uint32_t first_sequence, unsigned cycles,
                     unsigned fault_period) {
    unsigned completed = 0;
    unsigned injected_faults = 0;
    uint64_t start_us;
    uint64_t elapsed_us;

    if (cycles == 0 || cycles > 1000 || fault_period == 0) {
        puts("result protocol=link-feasibility-v1 status=soak_invalid_configuration");
        return;
    }
    start_us = time_us_64();
    while (completed < cycles) {
        /* A truncated slot is the repeatable recovery fault currently available
           without a human-operated power/reset fixture. Do not consume its
           error response; the following exchange must drain it as stale state. */
        if (completed != 0 && completed % fault_period == 0) {
            link_test_make_frame(&request_frame, (uint8_t)scenario,
                                 first_sequence + completed, 64,
                                 LINK_PATTERN_SLIP_BYTES);
            if (request_frame.status != LINK_STATUS_OK ||
                !wait_for(LINK_RP_READY_PIN, true, 1000)) break;
            transaction_bytes(&request_frame, &discard_frame, 32);
            /* The ESP validates the short slot after CS rises, then advertises
               its explicit error frame. Wait for that generation before calling
               exchange_quiet(): otherwise its stale-drain check can sample the
               old DATA_AVAILABLE low level and allow the next request to clock
               this error response instead of draining it. */
            exchange_failure = "partial_waiting_error_response";
            if (!wait_for(LINK_RP_DATA_AVAILABLE_PIN, true, 1000)) break;
            ++injected_faults;
        }
        if (!exchange_quiet(scenario, first_sequence + completed, 64,
                            LINK_PATTERN_FIXED_RANDOM, LINK_DATAPATH_POLLING, &(struct exchange_timing){0})) break;
        ++completed;
    }
    elapsed_us = time_us_64() - start_us;
    if (completed != cycles) {
        printf("result protocol=link-feasibility-v1 status=soak_failed completed=%u "
               "cycles=%u injected_faults=%u elapsed_us=%llu reason=%s\n", completed, cycles,
               injected_faults, (unsigned long long)elapsed_us, exchange_failure);
        return;
    }
    printf("result protocol=link-feasibility-v1 status=soak_pass cycles=%u "
           "injected_faults=%u elapsed_us=%llu\n", cycles, injected_faults,
           (unsigned long long)elapsed_us);
}

int main(void) {
    char line[64];

    /* Configure the physical SPI master and two ESP-to-RP flow-control inputs. */
    stdio_init_all();
    spi_init(spi0, LINK_SPI_BAUD_HZ);
    dma_tx_channel = dma_claim_unused_channel(true);
    dma_rx_channel = dma_claim_unused_channel(true);
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
        } else if (strncmp(line, "fault_partial", 13) == 0) {
            unsigned scenario = LINK_DEFAULT_SCENARIO;
            unsigned length = 64;
            unsigned pattern = LINK_PATTERN_INCREMENT;
            unsigned sequence = 1;
            unsigned slot_bytes = 32;
            (void)sscanf(line + 13, "%u %u %u %u %u", &scenario, &length,
                         &pattern, &sequence, &slot_bytes);
            run_fault_partial(scenario, sequence, length,
                              (enum link_test_pattern)pattern, slot_bytes);
        } else if (strncmp(line, "peer_reset", 10) == 0) {
            unsigned scenario = LINK_DEFAULT_SCENARIO;
            unsigned sequence = 1;
            (void)sscanf(line + 10, "%u %u", &scenario, &sequence);
            run_peer_reset(scenario, sequence);
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
        } else if (strncmp(line, "batch_profile", 13) == 0) {
            unsigned scenario = LINK_DEFAULT_SCENARIO;
            unsigned first_sequence = 1;
            unsigned count = 1;
            unsigned length = 64;
            unsigned pattern = LINK_PATTERN_INCREMENT;
            unsigned requested_hz = LINK_SPI_BAUD_HZ;
            char datapath[16] = "polling";
            (void)sscanf(line + 13, "%u %u %u %u %u %u %15s", &scenario,
                         &first_sequence, &count, &length, &pattern, &requested_hz,
                         datapath);
            if (strcmp(datapath, "polling") == 0) {
                run_batch(scenario, first_sequence, count, length,
                          (enum link_test_pattern)pattern, requested_hz,
                          LINK_DATAPATH_POLLING, true);
            } else if (strcmp(datapath, "dma") == 0) {
                run_batch(scenario, first_sequence, count, length,
                          (enum link_test_pattern)pattern, requested_hz,
                          LINK_DATAPATH_DMA, true);
            } else {
                puts("result protocol=link-feasibility-v1 status=batch_invalid_datapath");
            }
        } else if (strncmp(line, "batch", 5) == 0) {
            unsigned scenario = LINK_DEFAULT_SCENARIO;
            unsigned first_sequence = 1;
            unsigned count = 1;
            unsigned length = 64;
            unsigned pattern = LINK_PATTERN_INCREMENT;
            unsigned requested_hz = LINK_SPI_BAUD_HZ;
            (void)sscanf(line + 5, "%u %u %u %u %u %u", &scenario, &first_sequence,
                         &count, &length, &pattern, &requested_hz);
            run_batch(scenario, first_sequence, count, length,
                      (enum link_test_pattern)pattern, requested_hz,
                      LINK_DATAPATH_POLLING, false);
        } else if (strncmp(line, "soak", 4) == 0) {
            unsigned scenario = LINK_DEFAULT_SCENARIO;
            unsigned first_sequence = 1;
            unsigned cycles = 100;
            unsigned fault_period = 10;
            (void)sscanf(line + 4, "%u %u %u %u", &scenario, &first_sequence,
                         &cycles, &fault_period);
            run_soak(scenario, first_sequence, cycles, fault_period);
        } else if (strncmp(line, "pins", 4) == 0) {
            printf("pins sck=%d mosi=%d miso=%d cs=%d ready=%d data_available=%d\n", LINK_RP_SCK_PIN, LINK_RP_MOSI_PIN, LINK_RP_MISO_PIN, LINK_RP_CS_PIN, LINK_RP_READY_PIN, LINK_RP_DATA_AVAILABLE_PIN);
        } else if (strncmp(line, "help", 4) == 0) {
            puts("commands: run/oversize [scenario length pattern sequence], "
                 "receive [scenario sequence], "
                 "schedule [scenario outgoing_sequence request_sequence length pattern], "
                 "pressure [scenario length pattern sequence queue_depth pause_ms], "
                 "partial/fault_partial [scenario length pattern sequence slot_bytes], "
                 "peer_reset [scenario sequence], batch [scenario first count length pattern spi_hz], batch_profile [scenario first count length pattern spi_hz polling|dma], "
                 "soak [scenario first cycles fault_period], pins, help");
        } else {
            puts("error protocol=link-feasibility-v1 command");
        }
    }
}
