#include "hardware/clocks.h"
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
static void release(void *unused) {
    (void)unused;
    pio_sm_set_enabled(pio0, 0, false);
    /* SIO latch high before mux transfer; /AS deasserts before any release. */
    if (driving) {
        gpio_put(STIMULUS_AS_PIN, 1);
        gpio_set_dir(STIMULUS_AS_PIN, GPIO_OUT);
        gpio_set_function(STIMULUS_AS_PIN, GPIO_FUNC_SIO);
        busy_wait_us_32(10);
    }
    for (unsigned pin = STIMULUS_DATA_BASE; pin <= STIMULUS_AS_PIN; ++pin) {
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
    pio_sm_set_pins_with_mask(pio0, 0, 1u << STIMULUS_AS_PIN,
                              STIMULUS_PIN_MASK);
    pio_sm_set_pindirs_with_mask(pio0, 0, STIMULUS_PIN_MASK, STIMULUS_PIN_MASK);
    for (unsigned pin = STIMULUS_DATA_BASE; pin <= STIMULUS_AS_PIN; ++pin)
        pio_gpio_init(pio0, pin);
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
    for (unsigned pin = STIMULUS_DATA_BASE; pin <= STIMULUS_AS_PIN; ++pin) {
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
