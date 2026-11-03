/* Springboard bus service. The CS0/CS1 read path is mozzwald's hardware-proven
 * Experiment 2 engine (pio_rearm_program.c + persistent_reply.S) with its
 * Core1 service loop from his msc_main.c; only the CS1 reply source changed.
 * The write catcher, WE meter and select counters on PIO2 are input-only, so
 * they can never drive the bus. */
#include "bus.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "persistent_reply.h"
#include "pico/multicore.h"
#include "pio_rearm_program.h"
#include <apio.h>

/* The 8-byte offset keeps the proven table's modulo-16 SRAM bank alignment. */
static uint8_t table_backing[4 * BUS_TABLE_WORDS + 16]
    __attribute__((aligned(16)));
uint16_t *const bus_rom_words = (uint16_t *)(table_backing + 8);
uint16_t *const bus_cs1_words =
    (uint16_t *)(table_backing + 8) + BUS_TABLE_WORDS;

volatile uint32_t bus_write_ring[BUS_WRITE_RING_WORDS]
    __attribute__((aligned(BUS_WRITE_RING_WORDS * 4)));
static int write_dma = -1;

static volatile bool stop_request, arm_request, active, stopped = true;
static struct bus_totals totals;

void bus_pins_init(void) {
    const uint64_t mask = ((UINT64_C(1) << 47) - 1) & ~(UINT64_C(1) << 39);
    gpio_init_mask64(mask);
    gpio_set_dir_in_masked64(mask);
    for (unsigned p = 0; p <= 46; p++)
        if (p != 39)
            gpio_disable_pulls(p);
}

/* Force the data pads off whatever PIO thinks, e.g. before a restart. */
static void clamp(void) {
    pio1->irq_force = 1;
    for (unsigned pin = 20; pin <= 35; ++pin)
        gpio_set_oeover(pin, GPIO_OVERRIDE_LOW);
}

static void park(void) {
    pio_sm_set_enabled(pio1, 0, false);
    pio_sm_exec(pio1, 0, APIO_MOV_PINDIRS_NULL);
    pio_set_sm_mask_enabled(pio1, 15, false);
    pio_set_sm_mask_enabled(pio0, 7, false);
    for (unsigned sm = 0; sm < 4; ++sm) {
        pio_sm_clear_fifos(pio1, sm);
        pio_sm_restart(pio1, sm);
    }
    for (unsigned sm = 0; sm < 3; ++sm) {
        pio_sm_clear_fifos(pio0, sm);
        pio_sm_restart(pio0, sm);
    }
    pio_sm_exec(pio1, 0, APIO_SET_PINS(0));
}

static void start(void) {
    pio0->irq = pio1->irq = 0xff;
    pio_sm_exec(pio1, 0, APIO_SET_Y(10));
    pio_sm_exec(pio1, 0, APIO_JMP(SB_PR_MAIN));
    pio_sm_exec(pio1, 1, APIO_JMP(SB_PR_WATCH));
    pio_sm_exec(pio1, 2, APIO_JMP(SB_PR_OE));
    pio_sm_exec(pio1, 3, APIO_JMP(SB_PR_RESET));
    pio_sm_exec(pio0, 0, APIO_JMP(0));
    pio_sm_exec(pio0, 1, APIO_JMP(3));
    pio_sm_exec(pio0, 2, APIO_JMP(3));
    pio_enable_sm_mask_in_sync(pio0, 7);
    pio_enable_sm_mask_in_sync(pio1, 15);
}

static void bus_idle_wait(void) {
    /* Wait for CS0 and OE released and RESET high before taking the bus. */
    while (!stop_request && ((sio_hw->gpio_in & ((1u << 16) | (1u << 18))) !=
                                 ((1u << 16) | (1u << 18)) ||
                             !(sio_hw->gpio_hi_in & (1u << 13))))
        tight_loop_contents();
}

static void core1_service(void) {
    (void)save_and_disable_interrupts();
    const struct sb_cpu_bus bus = {&pio1->ctrl, &sio_hw->gpio_in,
                                   (const volatile bool *)&stop_request};
    for (;;) {
        sb_pio_rearm_program_init(false);
        park();
        for (unsigned pin = 20; pin <= 35; ++pin)
            gpio_set_oeover(pin, GPIO_OVERRIDE_NORMAL);
        ++totals.runs;
        stopped = false;
        bus_idle_wait();
        struct sb_persistent_totals t = {0};
        if (!stop_request) {
            start();
            __dmb();
            active = true;
            sb_persistent_reply(&t, bus_rom_words, &bus);
        }
        park();
        active = false;
        totals.requests += t.requests;
        totals.posted += t.posted;
        totals.completed += t.completed;
        totals.attempted += t.attempted;
        totals.cancelled += t.cancelled;
        totals.fault |= t.fault;
        __dmb();
        stopped = true;
        while (!arm_request)
            tight_loop_contents();
        arm_request = false;
        stop_request = false;
    }
}

void bus_load_rom(const uint8_t *rom, uint32_t size) {
    for (uint32_t i = 0; i < BUS_TABLE_WORDS; ++i) {
        uint32_t a = 2 * i;
        uint16_t hi = a < size ? rom[a] : 0xff,
                 lo = a + 1 < size ? rom[a + 1] : 0xff;
        bus_rom_words[i] = (uint16_t)(hi << 8 | lo);
    }
}

/* PIO2 runs with GPIO base 16, so one IN snapshot holds the selects (bits
 * 0-3), D0-D15 and A16-A23 together. SM0: wait for a CS1 write (CS0 high, CS1
 * low, OE high, WE low; Y = 5), let the data settle, snapshot, then wait for
 * WE to rise. Instruction 3 holds the settle delay. */
#define WRITE_SETTLE_PC 3
static unsigned write_settle = 8;

void bus_set_write_settle(unsigned cycles) {
    write_settle = cycles > 31 ? 31 : cycles;
    pio2->instr_mem[WRITE_SETTLE_PC] =
        pio_encode_nop() | pio_encode_delay(write_settle);
}

unsigned bus_write_settle(void) { return write_settle; }

void bus_write_capture_init(void) {
    pio_set_gpio_base(pio2, 16);
    pio2->instr_mem[0] = pio_encode_mov(pio_osr, pio_pins);
    pio2->instr_mem[1] = pio_encode_out(pio_x, 4);
    pio2->instr_mem[2] = pio_encode_jmp_x_ne_y(0);
    bus_set_write_settle(write_settle);
    pio2->instr_mem[4] = pio_encode_in(pio_pins, 32);
    pio2->instr_mem[5] = pio_encode_wait_pin(true, 3); /* WE */

    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, 0, 5);
    sm_config_set_in_pin_base(&c, 16);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv_int_frac8(&c, 1, 0);
    pio_sm_init(pio2, 0, 0, &c);
    pio_sm_exec(pio2, 0, pio_encode_set(pio_y, 5));

    write_dma = dma_claim_unused_channel(true);
    dma_channel_config d = dma_channel_get_default_config(write_dma);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_ring(&d, true, __builtin_ctz(sizeof bus_write_ring));
    channel_config_set_dreq(&d, pio_get_dreq(pio2, 0, false));
    dma_channel_configure(write_dma, &d, bus_write_ring, &pio2->rxf[0],
                          dma_encode_endless_transfer_count(), true);
    pio_sm_set_enabled(pio2, 0, true);

    /* SM1/SM2 count every CS1/CS0 cycle (wait low, wait high, X--). */
    pio2->instr_mem[8] = pio_encode_wait_pin(false, 0);
    pio2->instr_mem[9] = pio_encode_wait_pin(true, 0);
    pio2->instr_mem[10] = pio_encode_jmp_x_dec(8);
    for (unsigned sm = 1; sm <= 2; ++sm) {
        pio_sm_config e = pio_get_default_sm_config();
        sm_config_set_wrap(&e, 8, 10);
        sm_config_set_in_pin_base(&e, sm == 1 ? 17 : 16);
        sm_config_set_clkdiv_int_frac8(&e, 1, 0);
        pio_sm_init(pio2, sm, 8, &e);
        pio_sm_exec(pio2, sm, pio_encode_set(pio_x, 0));
        pio_sm_set_enabled(pio2, sm, true);
    }

    /* SM3 times WE low in each CS1 write: X counts down two cycles a loop
     * until WE (the JMP pin) rises, then X is pushed. */
    pio2->instr_mem[12] = pio_encode_mov(pio_osr, pio_pins);
    pio2->instr_mem[13] = pio_encode_out(pio_x, 4);
    pio2->instr_mem[14] = pio_encode_jmp_x_ne_y(12);
    pio2->instr_mem[15] = pio_encode_mov_not(pio_x, pio_null);
    pio2->instr_mem[16] = pio_encode_jmp_pin(18);
    pio2->instr_mem[17] = pio_encode_jmp_x_dec(16);
    pio2->instr_mem[18] = pio_encode_in(pio_x, 32);
    pio2->instr_mem[19] = pio_encode_wait_pin(true, 1); /* CS1 */
    pio_sm_config m = pio_get_default_sm_config();
    sm_config_set_wrap(&m, 12, 19);
    sm_config_set_in_pin_base(&m, 16);
    sm_config_set_jmp_pin(&m, 19);
    sm_config_set_in_shift(&m, false, true, 32);
    sm_config_set_out_shift(&m, true, false, 32);
    sm_config_set_clkdiv_int_frac8(&m, 1, 0);
    pio_sm_init(pio2, 3, 12, &m);
    pio_sm_exec(pio2, 3, pio_encode_set(pio_y, 5));
    pio_sm_set_enabled(pio2, 3, true);
}

bool bus_pop_we_width(uint32_t *ns) {
    if (pio_sm_is_rx_fifo_empty(pio2, 3))
        return false;
    uint32_t loops = ~pio_sm_get(pio2, 3);
    /* Two cycles a loop plus about three to see WE low and leave the loop. */
    *ns = (2 * loops + 3) * 1000u / (clock_get_hz(clk_sys) / 1000000u);
    return true;
}

static uint32_t edge_count(unsigned sm) {
    pio_sm_set_enabled(pio2, sm, false);
    pio_sm_exec(pio2, sm, pio_encode_mov(pio_isr, pio_x));
    pio_sm_exec(pio2, sm, pio_encode_push(false, false));
    uint32_t x = pio_sm_is_rx_fifo_empty(pio2, sm) ? 0 : pio_sm_get(pio2, sm);
    pio_sm_set_enabled(pio2, sm, true);
    return 0u - x;
}

void bus_select_counts(uint32_t *cs0, uint32_t *cs1) {
    *cs1 = edge_count(1);
    *cs0 = edge_count(2);
}

uint32_t bus_write_head(void) {
    uint32_t wa = dma_hw->ch[write_dma].write_addr;
    return ((wa - (uint32_t)(uintptr_t)bus_write_ring) / 4u) %
           BUS_WRITE_RING_WORDS;
}

void bus_launch_core1(void) {
    clamp();
    multicore_launch_core1(core1_service);
}

void bus_stop(void) { stop_request = true; }
void bus_rearm(void) {
    if (stopped)
        arm_request = true;
}
bool bus_is_active(void) { return active; }
void bus_get_totals(struct bus_totals *out) { *out = totals; }
