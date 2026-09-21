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

static struct link_test_frame request_frame;
static struct link_test_frame discard_frame;
static struct link_test_frame response_frame;
static struct link_test_frame zero_frame;

static bool wait_for(uint pin, bool value, uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (gpio_get(pin) != value) {
        if (time_reached(deadline)) return false;
        tight_loop_contents();
    }
    return true;
}

static void transaction(const struct link_test_frame *tx, struct link_test_frame *rx) {
    gpio_put(LINK_RP_CS_PIN, 0);
    spi_write_read_blocking(spi0, (const uint8_t *)tx, (uint8_t *)rx, sizeof(*tx));
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

static void run_once(unsigned scenario, size_t length, enum link_test_pattern pattern) {
    enum link_test_status status;
    if (!drain_stale_response()) {
        puts("result protocol=link-feasibility-v1 status=timeout_draining_stale_response");
        return;
    }
    link_test_make_frame(&request_frame, (uint8_t)scenario, 1, length, pattern);
    if (request_frame.status != LINK_STATUS_OK) {
        printf("result protocol=link-feasibility-v1 status=local_%s scenario=L%u length=%u\n",
               link_test_status_name((enum link_test_status)request_frame.status), scenario, (unsigned)length);
        return;
    }
    if (!wait_for(LINK_RP_READY_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=timeout_waiting_ready");
        return;
    }
    transaction(&request_frame, &discard_frame); /* slave validates request and prepares echo */
    if (!wait_for(LINK_RP_DATA_AVAILABLE_PIN, true, 1000)) {
        puts("result protocol=link-feasibility-v1 status=timeout_waiting_response");
        return;
    }
    memset(&discard_frame, 0, sizeof(discard_frame));
    transaction(&discard_frame, &response_frame);
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

int main(void) {
    char line[64];
    stdio_init_all();
    spi_init(spi0, LINK_SPI_BAUD_HZ);
    gpio_set_function(LINK_RP_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LINK_RP_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LINK_RP_MISO_PIN, GPIO_FUNC_SPI);
    gpio_init(LINK_RP_CS_PIN); gpio_set_dir(LINK_RP_CS_PIN, GPIO_OUT); gpio_put(LINK_RP_CS_PIN, 1);
    gpio_init(LINK_RP_READY_PIN); gpio_set_dir(LINK_RP_READY_PIN, GPIO_IN); gpio_pull_down(LINK_RP_READY_PIN);
    gpio_init(LINK_RP_DATA_AVAILABLE_PIN); gpio_set_dir(LINK_RP_DATA_AVAILABLE_PIN, GPIO_IN); gpio_pull_down(LINK_RP_DATA_AVAILABLE_PIN);
    sleep_ms(500);
    printf("ready protocol=link-feasibility-v1 default=L%d spi_hz=%u slot_bytes=%u\n",
           LINK_DEFAULT_SCENARIO, LINK_SPI_BAUD_HZ, (unsigned)sizeof(request_frame));
    for (;;) {
        if (fgets(line, sizeof(line), stdin) == NULL) { sleep_ms(10); continue; }
        if (strncmp(line, "run", 3) == 0) {
            unsigned scenario = LINK_DEFAULT_SCENARIO;
            unsigned length = 16;
            unsigned pattern = LINK_PATTERN_INCREMENT;
            (void)sscanf(line + 3, "%u %u %u", &scenario, &length, &pattern);
            run_once(scenario, length, (enum link_test_pattern)pattern);
        } else if (strncmp(line, "pins", 4) == 0) {
            printf("pins sck=%d mosi=%d miso=%d cs=%d ready=%d data_available=%d\n", LINK_RP_SCK_PIN, LINK_RP_MOSI_PIN, LINK_RP_MISO_PIN, LINK_RP_CS_PIN, LINK_RP_READY_PIN, LINK_RP_DATA_AVAILABLE_PIN);
        } else if (strncmp(line, "help", 4) == 0) {
            puts("commands: run [scenario 0..9] [length 0..240] [pattern 0..5], pins, help");
        } else {
            puts("error protocol=link-feasibility-v1 command");
        }
    }
}
