#include "link_protocol.h"

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#ifndef LINK_DEFAULT_SCENARIO
#define LINK_DEFAULT_SCENARIO 0
#endif
#ifndef LINK_ESP_SCLK_PIN
#define LINK_ESP_SCLK_PIN 12
#define LINK_ESP_MOSI_PIN 11
#define LINK_ESP_MISO_PIN 13
#define LINK_ESP_CS_PIN 10
#define LINK_ESP_READY_PIN 9
#define LINK_ESP_DATA_AVAILABLE_PIN 8
#endif

static const char *TAG = "link-lab";
static struct link_test_frame rx_frame __attribute__((aligned(4)));
static struct link_test_frame tx_frame __attribute__((aligned(4)));

void app_main(void) {
    spi_bus_config_t bus = {
        .mosi_io_num = LINK_ESP_MOSI_PIN, .miso_io_num = LINK_ESP_MISO_PIN,
        .sclk_io_num = LINK_ESP_SCLK_PIN, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(rx_frame),
    };
    spi_slave_interface_config_t slave = {
        .mode = 0, .spics_io_num = LINK_ESP_CS_PIN, .queue_size = 1,
    };
    ESP_ERROR_CHECK(gpio_set_direction(LINK_ESP_READY_PIN, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_direction(LINK_ESP_DATA_AVAILABLE_PIN, GPIO_MODE_OUTPUT));
    gpio_set_level(LINK_ESP_READY_PIN, 0); gpio_set_level(LINK_ESP_DATA_AVAILABLE_PIN, 0);
    ESP_ERROR_CHECK(spi_slave_initialize(SPI2_HOST, &bus, &slave, SPI_DMA_CH_AUTO));
    /* The native USB console disconnects across reset. Give the host a bounded
       re-enumeration window before emitting the machine-readable readiness line. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "ready protocol=link-feasibility-v1 default=L%d slot_bytes=%u", LINK_DEFAULT_SCENARIO, (unsigned)sizeof(rx_frame));
    for (;;) {
        spi_slave_transaction_t transfer = {0};
        spi_slave_transaction_t *completed = NULL;
        memset(&rx_frame, 0, sizeof(rx_frame));
        transfer.length = sizeof(rx_frame) * 8u;
        transfer.rx_buffer = &rx_frame;
        transfer.tx_buffer = &tx_frame;

        /* Queue the slot before raising either externally visible flow-control
           signal.  spi_slave_transmit() queues internally, so announcing READY
           first left a race where the RP2350 could clock an unqueued echo and
           receive the pull-down/idle zeroes on MISO. */
        ESP_ERROR_CHECK(spi_slave_queue_trans(SPI2_HOST, &transfer, portMAX_DELAY));
        gpio_set_level(LINK_ESP_READY_PIN, 1);
        gpio_set_level(LINK_ESP_DATA_AVAILABLE_PIN, tx_frame.magic == LINK_TEST_MAGIC);
        ESP_ERROR_CHECK(spi_slave_get_trans_result(SPI2_HOST, &completed, portMAX_DELAY));
        gpio_set_level(LINK_ESP_READY_PIN, 0);
        enum link_test_status status = link_test_validate_frame(&rx_frame);
        if (rx_frame.magic == 0 && tx_frame.magic == LINK_TEST_MAGIC) {
            /* RP2350 clocked out the prepared echo; consume it exactly once. */
            memset(&tx_frame, 0, sizeof(tx_frame));
            gpio_set_level(LINK_ESP_DATA_AVAILABLE_PIN, 0);
            continue;
        }
        link_test_make_echo(&rx_frame, &tx_frame, status);
        ESP_LOGI(TAG, "received scenario=L%u sequence=%lu length=%u status=%s",
                 rx_frame.scenario, (unsigned long)rx_frame.sequence, rx_frame.payload_length,
                 link_test_status_name(status));
    }
}
