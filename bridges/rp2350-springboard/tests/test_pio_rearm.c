/* Adapted from mozzwald/fujinet-nio experiment/dual-cs-write-capture,
 * bridges/rp2350-springboard/tests/test_pio_rearm.c: the read engine's PIO
 * program runs in epio against a model of Core1's request/reply loop. Added
 * here: CS1 reads (their own table) and write cycles, which must be ignored. */
#include "pio_rearm_program.h"
#include <apio.h>
#include <epio_priv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned test_which, test_phase, test_gap, test_late;
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "line%d cycle%u case%u/%u/%u/%u: %s\n", __LINE__,  \
                    s->t, test_which, test_phase, test_gap, test_late, #x);    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)
#define DATA (UINT64_C(0xffff) << 20)
#define MARKER (UINT64_C(1) << 39)
#define RESET (UINT64_C(1) << 45)
#define IDLE (RESET | (UINT64_C(15) << 16))
#define READ (RESET | (UINT64_C(10) << 16))  /* CS0 low, OE low */
#define READ1 (RESET | (UINT64_C(9) << 16))  /* CS1 low, OE low */
#define WRITE0 (RESET | (UINT64_C(6) << 16)) /* CS0 low, WE low, OE high */
#define WRITE1 (RESET | (UINT64_C(5) << 16)) /* CS1 low, WE low, OE high */
#define EXTERNAL (((UINT64_C(1) << 48) - 1) & ~(DATA | MARKER))
struct item {
    unsigned kind, owner, address;
};
struct sim {
    epio_t *e;
    uint16_t code[32];
    unsigned t, owner, address;
    unsigned snapshot_owner, snapshot_address, loaded_owner, loaded_word;
    struct item queue[16];
    unsigned head, tail;
    unsigned cpu_phase, cpu_owner, cpu_address, cpu_word, cpu_valid, due;
    unsigned latency, post, prolog, requests, posted, completed, attempted,
        cancelled, enables;
    unsigned released_at, max_release, overlap;
    unsigned last_end, last_selected_address, edge_attempts;
    bool selected, cs1, cpu_stall;
};
static bool driven(struct sim *s) {
    return (s->e->gpio.gpio_direction & DATA) != 0;
}
static uint16_t word(unsigned addr) {
    if (addr == 8)
        return 0xfeed;
    if (addr == 10)
        return 0xbeef;
    if (addr == 0x444)
        return 0xa174;
    if (addr == 0x446)
        return 0x5c8f;
    if (addr == 0x100)
        return 0;
    if (addr == 0x102)
        return 0xffff;
    return (uint16_t)((addr * 40503u) ^ (addr >> 3) ^ 0xa55a);
}
/* CS1 has its own table, as in persistent_reply.S. */
static uint16_t table(bool cs1, unsigned addr) {
    return cs1 ? (uint16_t)~word(addr) : word(addr);
}
static struct item pop(struct sim *s, unsigned kind) {
    CHECK(s->head != s->tail);
    struct item q = s->queue[s->head++ % 16];
    CHECK(q.kind == kind);
    return q;
}
static void cpu(struct sim *s) {
    if (s->cpu_stall)
        return;
    if (s->cpu_phase == 4 && s->t >= s->due)
        s->cpu_phase = 0;
    if (s->cpu_phase == 0 && epio_rx_fifo_depth(s->e, 1, 0)) {
        struct item q = pop(s, 0);
        uint32_t high = epio_pop_rx_fifo(s->e, 1, 0);
        s->cpu_owner = q.owner;
        s->cpu_address = q.address;
        s->cpu_valid = (high & 0x2000000f) == 0x2000000a ||
                       (high & 0x2000000f) == 0x20000009;
        s->cpu_word = table((high & 0xf) == 9,
                            (unsigned)s->e->gpio.gpio_input_state & 0xfffe);
        s->due = s->t + s->latency;
        s->cpu_phase = 1;
        ++s->requests;
    }
    if (s->cpu_phase == 1 && s->t >= s->due) {
        CHECK(!epio_tx_fifo_depth(s->e, 1, 0));
        if (!s->cpu_valid)
            epio_set_block_irq(s->e, 1, 0);
        else
            ++s->posted;
        epio_push_tx_fifo(s->e, 1, 0, s->cpu_valid ? s->cpu_word : 0);
        s->cpu_phase = 2;
        s->due = s->t + s->post;
    }
    if (s->cpu_phase == 2 && s->t >= s->due)
        s->cpu_phase = 3;
    if (s->cpu_phase == 3 && epio_rx_fifo_depth(s->e, 1, 0)) {
        (void)pop(s, 1);
        uint32_t token = epio_pop_rx_fifo(s->e, 1, 0);
        CHECK(token == 0 || token == UINT32_MAX);
        ++s->completed;
        if (token)
            ++s->attempted;
        else
            ++s->cancelled;
        s->cpu_phase = 4;
        s->due = s->t + s->prolog;
    }
}
static void step(struct sim *s, unsigned n) {
    while (n--) {
        cpu(s);
        epio_sm_state_t *main = &s->e->block[1].sm[0];
        unsigned pc = main->pc;
        CHECK(pc < SB_PR_WATCH ||
              (pc >= SB_PR_FALLBACK && pc < SB_PR_INSTRUCTIONS));
        bool executing = main->enabled && !main->delay;
        unsigned rx = epio_rx_fifo_depth(s->e, 1, 0),
                 tx = epio_tx_fifo_depth(s->e, 1, 0);
        bool before = driven(s);
        bool gate_cancel = epio_peek_block_irq_num(s->e, 1, 0);
        if (executing && pc == SB_PR_SNAPSHOT) {
            s->snapshot_owner = s->owner;
            s->snapshot_address = s->address;
        }
        if (executing && pc == SB_PR_PULL && tx) {
            s->loaded_owner = s->cpu_owner;
            s->loaded_word = s->cpu_valid ? s->cpu_word : 0;
        }
        if (executing && pc == SB_PR_PRELOAD)
            CHECK(!before);
        for (unsigned sm = 0; sm < 4; ++sm) {
            epio_sm_state_t *m = &s->e->block[1].sm[sm];
            if (m->enabled && !m->delay && (s->code[m->pc] & 0x1000)) {
                if (s->code[m->pc] & 0x800)
                    s->e->gpio.gpio_output_state |= MARKER;
                else
                    s->e->gpio.gpio_output_state &= ~MARKER;
            }
        }
        epio_step_cycles(s->e, 1);
        if (executing && pc == SB_PR_ENABLE && gate_cancel)
            CHECK(!driven(s));
        CHECK(epio_rx_fifo_depth(s->e, 1, 0) <= 2 &&
              epio_tx_fifo_depth(s->e, 1, 0) <= 1);
        if (epio_rx_fifo_depth(s->e, 1, 0) > rx) {
            CHECK(pc == SB_PR_REQUEST || pc == SB_PR_COMPLETE);
            CHECK(s->tail - s->head < 16);
            s->queue[s->tail++ % 16] = (struct item){
                pc == SB_PR_COMPLETE, s->snapshot_owner, s->snapshot_address};
        }
        CHECK(!(s->e->gpio.gpio_direction & ~(DATA | MARKER)));
        CHECK((s->e->gpio.gpio_direction & DATA) == 0 ||
              (s->e->gpio.gpio_direction & DATA) == DATA);
        if (!before && driven(s)) {
            if (!s->selected || s->loaded_owner != s->owner) {
                /* Visible-pin edge -> WAIT -> IRQ SET has a two-clock
                 * propagation window. Count rather than hide this limit. */
                CHECK(s->t - s->last_end <= 2);
                ++s->edge_attempts;
            } else
                CHECK(s->loaded_word == table(s->cs1, s->address));
            ++s->enables;
        }
        if (s->released_at && !driven(s)) {
            unsigned delay = s->t - s->released_at + 1;
            if (delay > s->max_release)
                s->max_release = delay;
            s->released_at = 0;
        }
        if (s->released_at)
            CHECK(s->t - s->released_at < 6);
        if (s->selected && driven(s) && s->loaded_owner != s->owner)
            ++s->overlap;
        ++s->t;
    }
}
static void bus(struct sim *s, uint64_t pins, unsigned addr, unsigned n) {
    uint64_t select = pins & ((uint64_t)15 << 16);
    bool cs1 = (pins & RESET) && select == ((uint64_t)9 << 16);
    bool selected = cs1 || ((pins & RESET) && select == ((uint64_t)10 << 16));
    if (s->selected && !selected) {
        s->last_end = s->t;
        if (driven(s))
            s->released_at = s->t;
    }
    if (selected && !s->selected)
        ++s->owner;
    s->selected = selected;
    s->cs1 = cs1;
    s->address = addr;
    if (selected)
        s->last_selected_address = addr;
    epio_drive_gpios_ext(s->e, EXTERNAL, pins | (addr & 0xfffe));
    step(s, n);
}
static void init(struct sim *s, unsigned latency, unsigned post,
                 unsigned prolog) {
    memset(s, 0, sizeof(*s));
    s->latency = latency;
    s->post = post;
    s->prolog = prolog;
    sb_pio_rearm_program_init(true);
    s->e = epio_init();
    CHECK(s->e);
    epio_set_gpio_output_control(s->e, 39, 1);
    epio_update_from_apio(s->e);
    for (unsigned i = 0; i < 32; ++i) {
        s->code[i] = epio_get_instr(s->e, 1, i);
        epio_set_instr(s->e, 1, i, s->code[i] & ~0x1800u);
    }
    CHECK((s->code[SB_PR_PRELOAD] & 0x1800u) ==
          (SB_PIO_REPLY_MARKER ? 0x1800u : 0));
    CHECK((s->code[SB_PR_ENABLE] & 0x1800u) ==
          (SB_PIO_REPLY_MARKER ? 0 : 0x1800u));
    bus(s, IDLE, 0, 40);
    CHECK(!driven(s));
}
static void pair(struct sim *s, unsigned addr, unsigned width, unsigned gap) {
    bus(s, READ, addr, width);
    CHECK(driven(s));
    CHECK(((s->e->gpio.gpio_output_state & DATA) >> 20) == word(addr));
    bus(s, IDLE, addr, gap);
    bus(s, READ, addr + 2, width);
    CHECK(driven(s));
    CHECK(((s->e->gpio.gpio_output_state & DATA) >> 20) == word(addr + 2));
    bus(s, IDLE, 0, 200);
}
int main(void) {
    struct sim storage, *s = &storage;
    init(s, 20, 16, 12);
    pair(s, 8, 62, 6);
    printf("basic: req%u completed%u enables%u attempted%u cancels%u release%u "
           "clocks\n",
           s->requests, s->completed, s->enables, s->attempted, s->cancelled,
           s->max_release);
    CHECK(s->requests == 2 && s->completed == 2 && s->enables == 2 &&
          !s->cancelled && !s->overlap);
    epio_free(s->e);
    unsigned cases = 0;
    for (unsigned gap = 1; gap <= 10; ++gap)
        for (unsigned latency = 0; latency <= 24; latency += 3)
            for (unsigned post = 0; post <= 24; post += 6)
                for (unsigned prolog = 0; prolog <= 24; prolog += 6) {
                    init(s, latency, post, prolog);
                    pair(s, 8, 62, gap);
                    CHECK(s->requests == 2 && s->completed == 2 &&
                          s->enables == 2 && !s->cancelled);
                    if (gap >= 5)
                        CHECK(!s->overlap);
                    epio_free(s->e);
                    ++cases;
                }
    printf("Continuous PIO paired reads: %u phases PASS; 5..7 clocks bracket "
           "measured gaps before input synchronization\n",
           cases);
    cases = 0;
    for (unsigned gap = 5; gap <= 7; ++gap)
        for (unsigned off = 0; off <= gap + 1; ++off)
            for (int lead = -2; lead <= 2; ++lead) {
                init(s, 20, 16, 12);
                bus(s, READ, 8, 62);
                CHECK(driven(s));
                for (unsigned t = 0; t < gap + 62; ++t) {
                    uint64_t pins = READ;
                    if (t < gap)
                        pins |= UINT64_C(1) << 16;
                    if ((int)t >= (int)off && (int)t < (int)gap + lead)
                        pins |= UINT64_C(1) << 18;
                    bus(s, pins, 10, 1);
                }
                CHECK(driven(s) &&
                      ((s->e->gpio.gpio_output_state & DATA) >> 20) == 0xbeef);
                bus(s, IDLE, 0, 200);
                CHECK(s->requests == 2 && s->completed == 2 &&
                      s->enables == 2 && !s->cancelled);
                epio_free(s->e);
                ++cases;
            }
    printf(
        "OE lead/lag/held-active across 5..7-clock CS0 gaps: %u cases PASS\n",
        cases);
    const uint64_t invalid[] = {
        READ | (UINT64_C(1) << 16),  READ | (UINT64_C(1) << 18),  READ & ~RESET,
        READ & ~(UINT64_C(1) << 19), READ & ~(UINT64_C(1) << 17), IDLE};
    cases = 0;
    unsigned edge_attempts = 0;
    for (unsigned which = 0; which < 6; ++which)
        for (unsigned phase = 0; phase < 80; ++phase)
            for (unsigned gap = 1; gap <= 7; ++gap)
                for (unsigned late = 0; late < 3; ++late) {
                    test_which = which;
                    test_phase = phase;
                    test_gap = gap;
                    test_late = late;
                    init(s, late == 0 ? 0 : late == 1 ? 20 : 100, 16, 12);
                    bus(s, READ, 8, phase);
                    bus(s, invalid[which], 8, gap);
                    bus(s, READ, 0x444, 70);
                    bus(s, IDLE, 0, 400);
                    CHECK(!driven(s) && s->requests == s->completed);
                    unsigned before = s->enables;
                    bus(s, READ, 0x2468, 180);
                    bus(s, IDLE, 0, 300);
                    CHECK(s->enables == before + 1 &&
                          s->requests == s->completed && !driven(s));
                    edge_attempts += s->edge_attempts;
                    epio_free(s->e);
                    ++cases;
                }
    printf("Cancellation/late-response: %u phases PASS; fresh recovery, %u "
           "attempts inside <=2-clock edge propagation window\n",
           cases, edge_attempts);
    for (unsigned phase = 0; phase < 80; ++phase) {
        init(s, 20, 16, 12);
        bus(s, READ, 8, phase);
        s->cpu_stall = true;
        bus(s, IDLE, 8, 1);
        bus(s, READ, 0x444, 1000);
        CHECK(!driven(s));
        s->cpu_stall = false;
        step(s, 300);
        bus(s, IDLE, 0, 100);
        bus(s, READ, 0x444, 100);
        bus(s, IDLE, 0, 300);
        CHECK(s->requests == s->completed && !driven(s));
        epio_free(s->e);
    }
    puts("CPU stalls at 80 phases: autonomous release, late response isolation "
         "and recovery PASS");
    init(s, 20, 16, 12);
    for (unsigned i = 0; i < 200; ++i) {
        /* Includes changing/repeated addresses and 0000/FFFF transitions via
         * the lookup function's native-word values, without idle cleanup. */
        const unsigned fixed[] = {8, 8, 0x444, 0x100, 0x102};
        unsigned address = i % 7 < 5 ? fixed[i % 7] : ((i * 254u) & 0xfffe);
        bus(s, READ, address, 62);
        bus(s, IDLE, address, 6);
    }
    bus(s, IDLE, 0, 200);
    CHECK(s->requests == 200 && s->completed == 200 && s->enables == 200 &&
          !s->cancelled && !s->overlap);
    epio_free(s->e);
    puts("200 changing/repeated addresses without CPU epoch cleanup PASS");
    init(s, 20, 16, 12);
    bus(s, READ, 8, 50);
    bus(s, READ & ~RESET, 8, 10000);
    CHECK(!driven(s));
    bus(s, IDLE, 0, 200);
    unsigned before = s->enables;
    bus(s, READ, 8, 100);
    bus(s, IDLE, 0, 200);
    CHECK(s->enables == before + 1 && !driven(s));
    epio_free(s->e);
    puts("Held RESET: no persistent drive; released RESET accepts fresh "
         "request PASS");
    init(s, 20, 16, 12);
    for (unsigned i = 0; i < 200; ++i) {
        /* CS0 and CS1 reads interleaved, each from its own table. */
        uint64_t select = i % 3 == 1 ? READ1 : READ;
        unsigned address = (i * 254u) & 0xfffe;
        bus(s, select, address, 62);
        CHECK(driven(s));
        CHECK(((s->e->gpio.gpio_output_state & DATA) >> 20) ==
              table(select == READ1, address));
        bus(s, IDLE, address, 6);
    }
    bus(s, IDLE, 0, 200);
    CHECK(s->requests == 200 && s->completed == 200 && s->enables == 200 &&
          !s->cancelled && !s->overlap);
    epio_free(s->e);
    puts("CS0 and CS1 reads from their own tables PASS");
    init(s, 20, 16, 12);
    for (unsigned i = 0; i < 40; ++i) {
        /* A write cycle is not a read: no request and nothing driven. */
        bus(s, i % 2 ? WRITE1 : WRITE0, 0x1234 + 2 * i, 50);
        CHECK(!driven(s));
        bus(s, IDLE, 0, 10);
    }
    CHECK(s->requests == 0 && s->enables == 0);
    bus(s, READ1, 0x40, 62);
    CHECK(driven(s) &&
          ((s->e->gpio.gpio_output_state & DATA) >> 20) == table(true, 0x40));
    bus(s, IDLE, 0, 200);
    CHECK(s->requests == 1 && s->completed == 1 && s->enables == 1 &&
          !s->cancelled && !driven(s));
    epio_free(s->e);
    puts("CS0 and CS1 write cycles ignored, a read still answered PASS");
    return 0;
}
