/* Serve a Springboard ROM on CS0 and a two-way link on CS1 of a Handspring
 * Visor, and carry the link to fujinet-nio over USB. */
#include "bus.h"
#include "console.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/powman.h"
#include "link.h"
#include "pico/stdlib.h"
#include "tusb.h"

extern const uint8_t sbnio_rom[];
extern const uint32_t sbnio_rom_size, sbnio_rom_crc;

static void announce(void) {
    static bool done;
    if (done || !tud_cdc_n_connected(1))
        return;
    done = true;
    console_printf("sbnio %s %s, %lu MHz, ROM %lu bytes crc %08lx\n", __DATE__,
                   __TIME__, (unsigned long)(clock_get_hz(clk_sys) / 1000000),
                   (unsigned long)sbnio_rom_size, (unsigned long)sbnio_rom_crc);
}

extern uint32_t link_chip_reset;

int main(void) {
    bus_pins_init();
    link_chip_reset = powman_hw->chip_reset;
    /* GP39 is the LED and TP3; the read engine side-sets it on every reply. */
    gpio_init(39);
    gpio_put(39, false);
    gpio_set_dir(39, GPIO_OUT);
    set_sys_clock_khz(150000, true);
    bus_load_rom(sbnio_rom, sbnio_rom_size);
    link_init();
    bus_write_capture_init();
    bus_launch_core1();
    tusb_init();
    console_init(link_command);
    for (;;) {
        tud_task();
        link_poll();
        announce();
        console_poll();
    }
}
