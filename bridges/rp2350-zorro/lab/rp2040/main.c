#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "stimulus.h"
#include "tusb.h"
#include <string.h>
/* Shared descriptor field positions must match the RP2040 SDK, not RP2350
 * MMIO or a separate host-only configuration. */
_Static_assert(PIO_SM0_CLKDIV_INT_LSB == 16 && PIO_SM0_CLKDIV_FRAC_LSB == 8,
               "RP2040 clock divider layout");
_Static_assert(PIO_SM0_EXECCTRL_WRAP_TOP_LSB == 12 &&
                   PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB == 7,
               "RP2040 wrap layout");
_Static_assert(PIO_SM0_PINCTRL_OUT_BASE_LSB == 0 &&
                   PIO_SM0_PINCTRL_SET_BASE_LSB == 5 &&
                   PIO_SM0_PINCTRL_OUT_COUNT_LSB == 20 &&
                   PIO_SM0_PINCTRL_SET_COUNT_LSB == 26,
               "RP2040 pin layout");
/* TinyUSB invokes these from the SDK background task. The main loop reads
 * epoch and connection state with interrupts disabled, through execution of
 * the command. A rapid DTR down/up therefore cannot masquerade as one session.
 */
static volatile uint32_t usb_epoch;
static bool usb_dtr;
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)rts;
    if (itf == 0 && dtr != usb_dtr) {
        usb_dtr = dtr;
        ++usb_epoch;
    }
}
void tud_mount_cb(void) { ++usb_epoch; }
void tud_umount_cb(void) {
    usb_dtr = false;
    ++usb_epoch;
}
static uint16_t words[STIMULUS_WORDS];
static pio_sm_config config;
static bool driving;
#if STIMULUS_USE_DMA
static int stimulus_dma;
static dma_channel_config stimulus_dma_config;
#endif
static void release(void *unused) {
    (void)unused;
    /* Preload the SIO data and direction latches while PIO still owns the
     * pins.  Switching the function first exposes reset-value SIO latches and
     * created a short false /AS assertion at the end of a W1 capture. */
    if (driving) {
        gpio_put_masked(STIMULUS_PIN_MASK, STIMULUS_IDLE_PIN_VALUES);
        for (unsigned pin = STIMULUS_OUTPUT_BASE;
             pin < STIMULUS_OUTPUT_BASE + STIMULUS_DRIVE_PINS; ++pin)
            gpio_set_dir(pin, GPIO_OUT);
    }
    pio_sm_set_enabled(pio0, 0, false);
#if STIMULUS_USE_DMA
    dma_channel_abort(stimulus_dma);
#endif
    /* The prepared SIO pins now take over at their declared idle value. */
    if (driving) {
        for (unsigned pin = STIMULUS_OUTPUT_BASE;
             pin < STIMULUS_OUTPUT_BASE + STIMULUS_DRIVE_PINS; ++pin) {
            gpio_set_function(pin, GPIO_FUNC_SIO);
        }
        busy_wait_us_32(10);
    }
    for (unsigned pin = STIMULUS_OUTPUT_BASE;
         pin < STIMULUS_OUTPUT_BASE + STIMULUS_DRIVE_PINS; ++pin) {
        gpio_set_dir(pin, GPIO_IN);
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_disable_pulls(pin);
    }
    gpio_pull_up(
        STIMULUS_AS_PIN); /* Weak idle bias; still input/high impedance. */
    driving = false;
    pio_interrupt_clear(pio0, 0);
}
static void start(void *unused) {
    (void)unused;
    driving = true;
    pio_sm_init(pio0, 0, 0, &config);
    pio_interrupt_clear(pio0, 0);
    pio_sm_set_pins_with_mask(pio0, 0, STIMULUS_IDLE_PIN_VALUES,
                              STIMULUS_PIN_MASK);
    /* Read fixtures give the PIO program ownership of output-enable.  The
       initial all-input state prevents the generator ever fighting DUT data
       drive while it waits for the first command word. */
#if STIMULUS_DYNAMIC_DIRECTIONS
    pio_sm_set_pindirs_with_mask(pio0, 0, 0, STIMULUS_PIN_MASK);
#else
    pio_sm_set_pindirs_with_mask(pio0, 0, STIMULUS_PIN_MASK, STIMULUS_PIN_MASK);
#endif
    for (unsigned pin = STIMULUS_OUTPUT_BASE;
         pin < STIMULUS_OUTPUT_BASE + STIMULUS_DRIVE_PINS; ++pin)
        pio_gpio_init(pio0, pin);
#if STIMULUS_USE_DMA
    dma_channel_set_read_addr(stimulus_dma, stimulus_output_words, false);
    dma_channel_set_trans_count(stimulus_dma, STIMULUS_OUTPUT_WORD_COUNT, false);
    dma_channel_start(stimulus_dma);
#endif
    pio_sm_set_enabled(pio0, 0, true);
}
/* Never let an unread USB console block the safety/control loop. Replies may
 * be dropped when the host stops reading; status retains the final result. */
static bool reply(void *unused, const char *s) {
    (void)unused;
    size_t n = strlen(s);
    if (!tud_cdc_connected() || tud_cdc_write_available() < n + 2)
        return false;
    tud_cdc_write(s, n);
    tud_cdc_write("\r\n", 2);
    tud_cdc_write_flush();
    return true;
}
static void flush_input(void *unused) {
    (void)unused;
    tud_cdc_read_flush();
}
static int read_byte(void *unused) {
    (void)unused;
    return tud_cdc_read_char();
}
int main(void) {
    /* Inputs without pulls from entry, including during USB initialization. */
    for (unsigned pin = STIMULUS_OUTPUT_BASE;
         pin < STIMULUS_OUTPUT_BASE + STIMULUS_DRIVE_PINS; ++pin) {
        gpio_init(pin);
        gpio_disable_pulls(pin);
    }
    stimulus_program(words);
    struct pio_program program = {
        .instructions = words, .length = STIMULUS_WORDS, .origin = 0};
    pio_add_program_at_offset(pio0, &program, 0);
    stimulus_registers shared = stimulus_config(clock_get_hz(clk_sys));
    config = (pio_sm_config){.clkdiv = shared.clkdiv,
                             .execctrl = shared.execctrl,
                             .shiftctrl = shared.shiftctrl,
                             .pinctrl = shared.pinctrl};
#if STIMULUS_USE_DMA
    stimulus_dma = dma_claim_unused_channel(true);
    stimulus_dma_config = dma_channel_get_default_config(stimulus_dma);
    channel_config_set_transfer_data_size(&stimulus_dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&stimulus_dma_config, true);
    channel_config_set_write_increment(&stimulus_dma_config, false);
    channel_config_set_dreq(&stimulus_dma_config, pio_get_dreq(pio0, 0, true));
    dma_channel_configure(stimulus_dma, &stimulus_dma_config, &pio0->txf[0],
                          stimulus_output_words, STIMULUS_OUTPUT_WORD_COUNT, false);
#endif
    stimulus_control state;
    stimulus_init(&state, start, release, NULL);
    stdio_init_all();
    stimulus_console console = {0};
    const stimulus_console_io io = {
        .flush = flush_input, .read_byte = read_byte, .try_reply = reply};
    while (true) {
        /* All TinyUSB FIFO access and the connected/epoch recheck are atomic
         * relative to its background task, including command execution. No
         * callback blocks: one byte, bounded parsing, short GPIO release. */
        uint32_t irq_state = save_and_disable_interrupts();
        bool connected = tud_cdc_connected();
        stimulus_console_step(&console, &state, &io, time_us_64(), connected,
                              usb_epoch, pio_interrupt_get(pio0, 0));
        restore_interrupts(irq_state);
        tight_loop_contents();
    }
}
