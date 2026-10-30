#include "link.h"
#include "bus.h"
#include "console.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/structs/powman.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "packet_link.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "sbnio.h"
#include "tusb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NIO_ITF 0

static uint8_t req[SBN_REQ_MAX];
static uint32_t req_len, req_expect;
static uint16_t req_check, req_palm_check;
static bool req_overflow, req_have_check;
static uint8_t tx[PL_HEADER + SBN_REQ_MAX + PL_CRC + 64];
static uint32_t tx_len, tx_pos, tx_dropped;
static struct pl_link nio;
static bool awaiting, loopback, trace, host;
static uint8_t seq_now;
static uint64_t sent_at;
static uint32_t write_tail, write_peak;
static uint32_t writes, unknown;
static uint32_t we_count, we_min = UINT32_MAX, we_max, we_last;
static char palm_log[96];
static unsigned palm_log_used;

static void reg(uint16_t offset, uint16_t value) {
    bus_cs1_words[offset / 2] = value;
}

static void publish_status(void) {
    uint16_t st = 0;
    if (host)
        st |= SBN_ST_HOST;
    if (awaiting)
        st |= SBN_ST_BUSY;
    if (loopback)
        st |= SBN_ST_LOOPBACK;
    reg(SBN_R_STATUS, st);
}

static const char *result_name(uint16_t r) {
    static const char *const names[] = {"ok",         "no-host",    "timeout",
                                        "bad-length", "bad-check",  "too-big",
                                        "aborted",    "unanswered", "refused"};
    return r < sizeof names / sizeof names[0] ? names[r] : "?";
}

/* Data first, then length, result and check, then the sequence the Palm polls.
 */
static void publish_reply(uint8_t seq, uint16_t result, const uint8_t *data,
                          uint32_t len) {
    uint16_t check = 0;
    for (uint32_t i = 0; i < len; i += 2) {
        uint16_t w = (uint16_t)(data[i] << 8 | (i + 1 < len ? data[i + 1] : 0));
        bus_cs1_words[SBN_R_RESP / 2 + i / 2] = w;
        check = SBN_CHECK_STEP(check, w);
    }
    reg(SBN_R_RESP_LEN, (uint16_t)len);
    reg(SBN_R_RESULT, result);
    reg(SBN_R_RESP_CHECK, check);
    __dmb();
    reg(SBN_R_RESP_SEQ, seq);
    awaiting = false;
    publish_status();
    console_printf("#%u reply %s, %lu bytes, %lu ms\n", seq,
                   result_name(result), (unsigned long)len,
                   (unsigned long)((time_us_64() - sent_at) / 1000));
    if (len)
        console_hex("  <", data, len < 64 ? len : 64);
}

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* The packet link's output, drained to USB by pump_usb. */
static void nio_send(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    if (tx_pos == tx_len)
        tx_pos = tx_len = 0;
    if (tx_len + len > sizeof tx) {
        ++tx_dropped;
        return;
    }
    memcpy(tx + tx_len, data, len);
    tx_len += (uint32_t)len;
}

static void nio_done(void *ctx, enum pl_outcome outcome, const uint8_t *body,
                     size_t len) {
    static const uint16_t result[] = {
        [PL_ANSWERED] = SBN_RES_OK,           [PL_REFUSED] = SBN_RES_REFUSED,
        [PL_UNANSWERED] = SBN_RES_UNANSWERED, [PL_TIMED_OUT] = SBN_RES_TIMEOUT,
        [PL_NO_PEER] = SBN_RES_NO_HOST,       [PL_CANCELLED] = SBN_RES_ABORTED,
    };
    (void)ctx;
    if (!awaiting)
        return;
    if (len > SBN_RESP_MAX)
        return publish_reply(seq_now, SBN_RES_TOO_BIG, NULL, 0);
    publish_reply(seq_now, result[outcome], body, len);
}

/* Send req to NIO, or answer with the verdict if the request was bad. */
static void doorbell(uint8_t seq, uint16_t verdict) {
    reg(SBN_R_ACK_SEQ, seq);
    sent_at = time_us_64();
    uint32_t len = req_len;
    if (awaiting) {
        console_printf("#%u abandons #%u\n", seq, seq_now);
        awaiting = false;
        pl_cancel(&nio, now_ms());
    }
    console_printf("#%u request %lu bytes\n", seq, (unsigned long)len);
    if (len)
        console_hex("  >", req, len < 64 ? len : 64);
    if (verdict != SBN_RES_OK)
        return publish_reply(seq, verdict, NULL, 0);
    if (loopback)
        return publish_reply(seq, SBN_RES_OK, req, len);
    if (!host)
        return publish_reply(seq, SBN_RES_NO_HOST, NULL, 0);
    seq_now = seq;
    awaiting = true;
    publish_status();
    pl_request(&nio, req, len, now_ms());
}

static void palm_log_char(uint8_t c) {
    if (c == '\n' || palm_log_used == sizeof palm_log - 1) {
        palm_log[palm_log_used] = 0;
        console_printf("palm: %s\n", palm_log);
        palm_log_used = 0;
    }
    if (c != '\n')
        palm_log[palm_log_used++] = (char)c;
}

static void control(uint16_t value) {
    switch (value) {
    case SBN_CTL_ABORT:
        pl_cancel(&nio, now_ms());
        break;
    case SBN_CTL_LOOPBACK_ON:
        loopback = true;
        break;
    case SBN_CTL_LOOPBACK_OFF:
        loopback = false;
        break;
    }
    console_printf("control %u\n", value);
    publish_status();
}

/* The request words must match what BEGIN and CHECK announced. */
static uint16_t request_verdict(void) {
    uint32_t want = (req_expect + 1) & ~1u;
    if (!req_expect || req_expect > SBN_REQ_MAX || req_overflow ||
        req_len != want)
        return SBN_RES_BAD_LEN;
    if (!req_have_check || req_check != req_palm_check) {
        console_printf("request check: palm %04x%s, firmware %04x\n",
                       req_palm_check, req_have_check ? "" : " (not sent)",
                       req_check);
        return SBN_RES_BAD_CHECK;
    }
    req_len = req_expect;
    return SBN_RES_OK;
}

static void handle_write(uint32_t snapshot) {
    uint8_t r = BUS_WRITE_REG(snapshot);
    uint16_t data = BUS_WRITE_DATA(snapshot);
    ++writes;
    if (trace)
        console_printf("write %u %04x (%08lx)\n", r, data,
                       (unsigned long)snapshot);
    switch (r) {
    case SBN_W_BEGIN:
        req_len = 0;
        req_expect = data;
        req_check = 0;
        req_overflow = req_have_check = false;
        break;
    case SBN_W_DATA:
        if (req_len + 2 <= SBN_REQ_MAX) {
            req[req_len++] = (uint8_t)(data >> 8);
            req[req_len++] = (uint8_t)data;
        } else
            req_overflow = true;
        req_check = SBN_CHECK_STEP(req_check, data);
        break;
    case SBN_W_CHECK:
        req_palm_check = data;
        req_have_check = true;
        break;
    case SBN_W_DOORBELL:
        doorbell((uint8_t)data, request_verdict());
        break;
    case SBN_W_CONTROL:
        control(data);
        break;
    case SBN_W_LOG:
        if (data >> 8)
            palm_log_char((uint8_t)(data >> 8));
        if (data & 0xff)
            palm_log_char((uint8_t)data);
        break;
    case SBN_W_ECHO:
        reg(SBN_R_ECHO, data);
        break;
    default:
        ++unknown;
        break;
    }
}

static void drain_writes(void) {
    uint32_t head = bus_write_head();
    uint32_t fill = (head - write_tail) % BUS_WRITE_RING_WORDS;
    if (fill > write_peak)
        write_peak = fill;
    if (!fill)
        return;
    while (write_tail != head) {
        handle_write(bus_write_ring[write_tail]);
        write_tail = (write_tail + 1) % BUS_WRITE_RING_WORDS;
    }
    /* Once per batch, to keep Core0 off the table Core1 is serving. */
    reg(SBN_R_WRITES, (uint16_t)writes);
    reg(SBN_R_REQ_BYTES, (uint16_t)req_len);
}

static void drain_widths(void) {
    uint32_t ns;
    while (bus_pop_we_width(&ns)) {
        ++we_count;
        we_last = ns;
        if (ns < we_min)
            we_min = ns;
        if (ns > we_max)
            we_max = ns;
    }
}

static void pump_usb(void) {
    bool now = tud_cdc_n_connected(NIO_ITF);
    if (now != host) {
        host = now;
        console_printf("NIO port %s\n", host ? "open" : "closed");
        tx_len = tx_pos = 0;
        if (host)
            pl_open(&nio, now_ms());
        else
            pl_close(&nio);
        publish_status();
    }
    while (tx_pos < tx_len) {
        uint32_t room = tud_cdc_n_write_available(NIO_ITF);
        if (!room)
            break;
        uint32_t n = tx_len - tx_pos;
        tx_pos += tud_cdc_n_write(NIO_ITF, tx + tx_pos, n < room ? n : room);
    }
    tud_cdc_n_write_flush(NIO_ITF);
    enum pl_state was = nio.state;
    uint8_t buf[64];
    while (tud_cdc_n_available(NIO_ITF)) {
        uint32_t n = tud_cdc_n_read(NIO_ITF, buf, sizeof buf);
        pl_receive(&nio, buf, n, now_ms());
    }
    pl_poll(&nio, now_ms());
    if (was == PL_AWAIT_ACK && nio.state >= PL_READY)
        console_printf("NIO link generation %u: version %u, capacity %u, "
                       "record timeout %u ms\n",
                       nio.generation, nio.version, nio.capacity,
                       nio.peer_timeout_ms);
    else if (was >= PL_READY && nio.state == PL_QUIET)
        console_printf("NIO link resyncing\n");
}

void link_init(void) {
    nio.send = nio_send;
    nio.done = nio_done;
    pl_init(&nio);
    for (uint32_t i = 0; i < BUS_TABLE_WORDS; ++i)
        bus_cs1_words[i] = 0;
    reg(SBN_R_MAGIC, SBN_MAGIC_VALUE);
    reg(SBN_R_VERSION, SBN_ABI_VERSION);
    publish_status();
}

void link_poll(void) {
    static uint32_t last_ms;
    drain_writes();
    drain_widths();
    pump_usb();
    /* Once a millisecond: Core0 stays off the table while Core1 reads it. */
    uint32_t ms = to_ms_since_boot(get_absolute_time());
    if (ms != last_ms) {
        last_ms = ms;
        reg(SBN_R_TICKS, (uint16_t)ms);
    }
}

uint32_t link_chip_reset;

static const char *reset_reason(void) {
    uint32_t r = link_chip_reset;
    if (r & POWMAN_CHIP_RESET_HAD_BOR_BITS)
        return "brown-out";
    if (r & POWMAN_CHIP_RESET_HAD_POR_BITS)
        return "power-on";
    if (r & POWMAN_CHIP_RESET_HAD_RUN_LOW_BITS)
        return "RUN pin";
    if (r & POWMAN_CHIP_RESET_HAD_GLITCH_DETECT_BITS)
        return "glitch detector";
    if (r & (POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_PSM_BITS |
             POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_SWCORE_BITS |
             POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_BITS |
             POWMAN_CHIP_RESET_HAD_WATCHDOG_RESET_POWMAN_ASYNC_BITS))
        return "watchdog/reboot";
    if (r & POWMAN_CHIP_RESET_HAD_DP_RESET_REQ_BITS)
        return "debugger";
    return "unknown";
}

static void print_widths(void) {
    if (!we_count)
        return console_printf("WE low: no CS1 writes measured\n");
    console_printf("WE low ns: %lu writes, min %lu max %lu last %lu\n",
                   (unsigned long)we_count, (unsigned long)we_min,
                   (unsigned long)we_max, (unsigned long)we_last);
}

static void print_settle(void) {
    unsigned mhz = clock_get_hz(clk_sys) / 1000000u,
             cycles = bus_write_settle();
    console_printf("write settle %u cycles, %u ns at %u MHz\n", cycles,
                   cycles * 1000u / mhz, mhz);
}

static void status(void) {
    struct bus_totals t;
    uint32_t cs0, cs1;
    bus_get_totals(&t);
    bus_select_counts(&cs0, &cs1);
    /* Core1 publishes its own totals only when it stops; the PIO2 edge
     * counters are live. */
    console_printf(
        "up %lu s, last reset: %s\n",
        (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000),
        reset_reason());
    console_printf(
        "bus %s, CS0 cycles %lu, CS1 cycles %lu (runs %lu, fault %lu)\n",
        bus_is_active() ? "active" : "stopped", (unsigned long)cs0,
        (unsigned long)cs1, (unsigned long)t.runs, (unsigned long)t.fault);
    console_printf("writes %lu unknown %lu ring-peak %lu, request %lu bytes\n",
                   (unsigned long)writes, (unsigned long)unknown,
                   (unsigned long)write_peak, (unsigned long)req_len);
    print_widths();
    print_settle();
    console_printf("nio port %s, link %s gen %u cap %u, %s, loopback %s\n",
                   host ? "open" : "closed", pl_state_name(nio.state),
                   nio.generation, nio.capacity, awaiting ? "waiting" : "idle",
                   loopback ? "on" : "off");
    const struct pl_counters *c = &nio.count;
    console_printf(
        "link syncs %lu (%lu unanswered), requests %lu answers %lu refused %lu "
        "unanswered %lu timeouts %lu resends %lu corrupt %lu tx-dropped %lu\n",
        (unsigned long)c->syncs, (unsigned long)c->sync_timeouts,
        (unsigned long)c->requests, (unsigned long)c->answers,
        (unsigned long)c->refused, (unsigned long)c->unanswered,
        (unsigned long)c->timeouts, (unsigned long)c->resends,
        (unsigned long)c->corrupt, (unsigned long)tx_dropped);
}

static void pins(void) {
    uint32_t lo = sio_hw->gpio_in, hi = sio_hw->gpio_hi_in;
    uint32_t a =
        (lo >> 1 & 0x7fffu) | (hi >> 4 & 7u) << 15 | (hi >> 8 & 0x1fu) << 18;
    uint32_t d = (lo >> 20 & 0xfffu) | (hi & 0xfu) << 12;
    console_printf("CS0 %lu CS1 %lu OE %lu WE %lu RESET %lu LOWBAT %lu IRQ %lu "
                   "A %06lx D %04lx\n",
                   lo >> 16 & 1, lo >> 17 & 1, lo >> 18 & 1, lo >> 19 & 1,
                   hi >> 13 & 1, hi >> 14 & 1, lo & 1, (unsigned long)a << 1,
                   (unsigned long)d);
}

/* Count level changes on the control lines for 200 ms. A seated module on a
 * running Prism shows OE toggling constantly (it is shared with the Prism's
 * own flash); a dark or badly seated slot shows nothing. */
static void watch(void) {
    static const uint8_t pin[] = {16, 17, 18, 19, 45, 46, 0};
    static const char *const name[] = {"CS0",   "CS1",    "OE", "WE",
                                       "RESET", "LOWBAT", "IRQ"};
    uint32_t changes[7] = {0}, lows[7] = {0}, samples = 0;
    uint64_t prev = gpio_get_all64(), end = time_us_64() + 200000;
    while (time_us_64() < end) {
        uint64_t now = gpio_get_all64(), diff = now ^ prev;
        for (unsigned i = 0; i < 7; ++i) {
            changes[i] += (uint32_t)(diff >> pin[i] & 1);
            lows[i] += (uint32_t)(~now >> pin[i] & 1);
        }
        prev = now;
        ++samples;
    }
    for (unsigned i = 0; i < 7; ++i)
        console_printf("%-6s changes %7lu  low %3lu%%\n", name[i],
                       (unsigned long)changes[i],
                       (unsigned long)(100ull * lows[i] / samples));
}

/* Count level changes on every GPIO for the given milliseconds, to see which
 * pins move while the Palm does something. */
static void scan(const char *arg) {
    uint32_t ms = (uint32_t)atoi(arg);
    uint32_t changes[48] = {0};
    if (!ms || ms > 20000)
        ms = 3000;
    uint64_t prev = gpio_get_all64(), end = time_us_64() + 1000ull * ms;
    while (time_us_64() < end) {
        uint64_t now = gpio_get_all64(), diff = now ^ prev;
        while (diff) {
            unsigned pin = (unsigned)__builtin_ctzll(diff);
            ++changes[pin];
            diff &= diff - 1;
        }
        prev = now;
    }
    console_printf("scan %lu ms:", (unsigned long)ms);
    for (unsigned pin = 0; pin < 48; ++pin)
        if (changes[pin])
            console_printf(" GP%u=%lu", pin, (unsigned long)changes[pin]);
    console_printf("\n");
}

/* With the bus engine stopped, pull each pin down with its input buffer off
 * (RP2350-E9 leakage otherwise holds a floating pad near 2 V), then read it.
 * A line the Prism drives high stays 1; an unconnected one reads 0. */
static void float_test(void) {
    static const uint8_t pin[] = {16, 17, 18, 19, 20, 45};
    static const char *const name[] = {"CS0", "CS1", "OE", "WE", "D0", "RESET"};
    if (bus_is_active()) {
        console_printf("float: run \"stop\" first\n");
        return;
    }
    for (unsigned i = 0; i < 6; ++i) {
        unsigned p = pin[i], ones = 0;
        uint32_t saved = pads_bank0_hw->io[p];
        for (unsigned k = 0; k < 8; ++k) {
            hw_clear_bits(&pads_bank0_hw->io[p], PADS_BANK0_GPIO0_IE_BITS);
            hw_set_bits(&pads_bank0_hw->io[p], PADS_BANK0_GPIO0_PDE_BITS);
            busy_wait_us(200);
            hw_set_bits(&pads_bank0_hw->io[p], PADS_BANK0_GPIO0_IE_BITS);
            busy_wait_at_least_cycles(20);
            ones += gpio_get(p);
        }
        pads_bank0_hw->io[p] = saved;
        console_printf("%-5s pulled down: high %u/8, %s\n", name[i], ones,
                       ones == 8 ? "driven high"
                       : ones    ? "toggling"
                                 : "floating or held low");
    }
}

/* "send <hex>": queue a raw FujiBus packet as if the Palm had sent it, under
 * sequence 0, which the Palm never uses. */
static void send_hex(const char *hex) {
    uint32_t n = 0;
    while (hex[0] && hex[1] && n < SBN_REQ_MAX) {
        unsigned v;
        if (sscanf(hex, "%2x", &v) != 1)
            break;
        req[n++] = (uint8_t)v;
        hex += 2;
        while (*hex == ' ')
            ++hex;
    }
    req_len = n;
    doorbell(0, n ? SBN_RES_OK : SBN_RES_BAD_LEN);
}

static void settle(const char *arg) {
    char *end;
    long v = strtol(arg, &end, 10);
    if (end == arg || *end || v < 0 || v > 31)
        console_printf("settle <0-31>\n");
    else
        bus_set_write_settle((unsigned)v);
    print_settle();
}

static void widths(void) {
    print_widths();
    we_count = we_max = we_last = 0;
    we_min = UINT32_MAX;
}

void link_command(const char *line) {
    if (!strcmp(line, "status"))
        status();
    else if (!strcmp(line, "pins"))
        pins();
    else if (!strcmp(line, "watch"))
        watch();
    else if (!strncmp(line, "scan", 4))
        scan(line + 4);
    else if (!strcmp(line, "float"))
        float_test();
    else if (!strncmp(line, "send ", 5))
        send_hex(line + 5);
    else if (!strcmp(line, "regs")) {
        for (unsigned r = SBN_R_MAGIC; r <= SBN_R_LAST; r += 2)
            console_printf("%04x: %04x\n", r, bus_cs1_words[r / 2]);
    } else if (!strcmp(line, "settle"))
        print_settle();
    else if (!strncmp(line, "settle ", 7))
        settle(line + 7);
    else if (!strcmp(line, "widths"))
        widths();
    else if (!strcmp(line, "trace on"))
        trace = true;
    else if (!strcmp(line, "trace off"))
        trace = false;
    else if (!strcmp(line, "loopback on")) {
        loopback = true;
        publish_status();
    } else if (!strcmp(line, "loopback off")) {
        loopback = false;
        publish_status();
    } else if (!strcmp(line, "stop"))
        bus_stop();
    else if (!strcmp(line, "arm"))
        bus_rearm();
    else if (!strcmp(line, "bootsel"))
        reset_usb_boot(0, 0);
    else
        console_printf(
            "commands: status pins watch scan [ms] float regs send <hex> "
            "trace on|off loopback on|off settle [0-31] widths stop arm "
            "bootsel\n");
}
