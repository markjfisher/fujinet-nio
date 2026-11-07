/* Bench only: no Zorro wiring, no bus outputs, no automatic flashing. */
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "tusb.h"
#include "probe.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct pl_link link;
static struct probe_queue tx;
static uint8_t data_port = 255, log_port = 255;
static bool fault, reported_fault, clock_pending, auto_clock, last_ok;
static uint32_t started, tx_progress;
static char command[2 * PL_MAX_BODY + 16];
static size_t command_len;
static bool command_overflow;
static uint8_t request[PL_MAX_BODY];
static uint32_t log_bytes;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void queue_send(void *ctx, const uint8_t *data, size_t n) {
    (void)ctx;
    if (!tx.used) tx_progress = now_ms();
    if (!probe_append(&tx, data, n)) fault = true;
}

static void completion(void *ctx, enum pl_outcome outcome,
                       const uint8_t *body, size_t n) {
    (void)ctx;
    printf("completion outcome=%u bytes=%u elapsed_ms=%" PRIu32
           " crc32=%08" PRIx32 "\n", outcome, (unsigned)n,
           now_ms() - started, pl_crc32(body, n, 0));
    last_ok = false;
    if (outcome == PL_ANSWERED) {
        if (clock_pending) {
            uint8_t status;
            uint64_t seconds;
            last_ok = probe_clock(body, n, &status, &seconds);
            if (last_ok)
                printf("clock valid status=%u unix_seconds=%" PRIu64
                       " (nonzero status is a service error)\n", status, seconds);
            else puts("FAIL invalid Clock.GetTime response");
        } else {
            last_ok = probe_raw_valid(body, n);
            printf("raw length/checksum=%s prefix=", last_ok ? "ok" : "FAIL");
            for (size_t i = 0; i < n && i < 32; ++i) printf("%02x", body[i]);
            puts(n > 32 ? "..." : "");
        }
    }
    clock_pending = false;
    /* Latch uncertain outcomes. An operator starts a NEW exchange explicitly. */
    if (outcome == PL_TIMED_OUT || outcome == PL_UNANSWERED) fault = true;
}

static void status(void) {
    printf("state=%s data=%u logs=%u generation=%u capacity=%u tx=%u fault=%u "
           "requests=%" PRIu32 " answers=%" PRIu32 " resends=%" PRIu32
           " timeouts=%" PRIu32 " corrupt=%" PRIu32 " log_bytes=%" PRIu32 "\n",
           pl_state_name(link.state), data_port, log_port, link.generation,
           link.capacity, (unsigned)tx.used, fault, link.count.requests,
           link.count.answers, link.count.resends, link.count.timeouts,
           link.count.corrupt, log_bytes);
}

static void restart(void) {
    pl_close(&link);
    tx = (struct probe_queue){0};
    if (data_port != 255) {
        tuh_cdc_write_clear(data_port);
        tuh_cdc_read_clear(data_port);
        pl_open(&link, now_ms());
    }
    fault = reported_fault = last_ok = false;
    auto_clock = false;
}

void tuh_cdc_mount_cb(uint8_t idx) {
    tuh_itf_info_t info;
    if (!tuh_cdc_itf_get_info(idx, &info)) return;
    printf("CDC mount index=%u address=%u interface=%u\n",
           idx, info.daddr, info.desc.bInterfaceNumber);
    /* ESP profile: CDC0's control interface is 0, CDC1's is 2. */
    if (info.desc.bInterfaceNumber == 0 && data_port == 255) {
        data_port = idx;
        restart();
        auto_clock = true;
    } else if (info.desc.bInterfaceNumber == 2) log_port = idx;
}

void tuh_cdc_umount_cb(uint8_t idx) {
    printf("CDC unmount index=%u\n", idx);
    if (idx == data_port) {
        data_port = 255;
        pl_close(&link);
        tx = (struct probe_queue){0};
        auto_clock = last_ok = false;
    }
    if (idx == log_port) log_port = 255;
}

static bool can_submit(void) {
    if (fault || link.state != PL_READY || link.pending || tx.used) {
        puts("not ready: use status; after fault use resync then a NEW request");
        return false;
    }
    return true;
}

static void clock_request(void) {
    if (!can_submit()) return;
    started = now_ms();
    clock_pending = true;
    pl_request(&link, probe_clock_request, sizeof probe_clock_request, started);
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void execute(void) {
    if (!strcmp(command, "status")) status();
    else if (!strcmp(command, "clock")) clock_request();
    else if (!strcmp(command, "resync")) restart();
    else if (!strcmp(command, "bootsel")) {
        puts("Entering ROM device mode; power off and select Flash RP wiring.");
        sleep_ms(10);
        reset_usb_boot(0, 0);
    } else if (!strncmp(command, "send ", 5)) {
        size_t n = strlen(command + 5);
        if (!n || n % 2 || n / 2 > sizeof request) {
            puts("invalid hex length"); return;
        }
        for (size_t i = 0; i < n / 2; ++i) {
            int a = hex_digit(command[5 + 2*i]);
            int b = hex_digit(command[6 + 2*i]);
            if (a < 0 || b < 0) { puts("invalid hex digit"); return; }
            request[i] = (uint8_t)(16*a+b);
        }
        if (!probe_raw_valid(request, n / 2)) {
            puts("invalid raw length/checksum"); return;
        }
        if (!can_submit()) return;
        started = now_ms();
        pl_request(&link, request, n / 2, started);
    } else puts("commands: status | clock | send HEX | resync | bootsel");
}

int main(void) {
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    link.send = queue_send;
    link.done = completion;
    link.answer_timeout_ms = 3000;
    pl_init(&link);
    puts("USB host probe: UART0 GP0 TX / GP1 RX, 115200 8N1; NO AMIGA");
    if (!tuh_init(0)) panic("USB host init failed");
    while (true) {
        tuh_task();
        uint32_t now = now_ms();
        uint8_t rx[256];
        /* Bounded draining; logs never enter PacketLink. */
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t n = data_port == 255 ? 0 : tuh_cdc_read(data_port, rx, sizeof rx);
            if (!n) break;
            if (!fault) pl_receive(&link, rx, n, now);
        }
        if (log_port != 255) log_bytes += tuh_cdc_read(log_port, rx, sizeof rx);
        if (!fault && data_port != 255) {
            pl_poll(&link, now);
            const uint8_t *p;
            size_t n = probe_front(&tx, &p);
            if (n) {
                uint32_t wrote = tuh_cdc_write(data_port, p, n);
                if (wrote) { probe_consume(&tx, wrote); tx_progress = now; }
                tuh_cdc_write_flush(data_port);
                if (now - tx_progress >= 1000) fault = true;
            }
            if (auto_clock && link.state == PL_READY && !tx.used) {
                auto_clock = false;
                clock_request();
            }
        }
        if (fault && !reported_fault) {
            reported_fault = true;
            pl_close(&link);
            tx = (struct probe_queue){0};
            if (data_port != 255) tuh_cdc_write_clear(data_port);
            puts("FAULT latched; uncertain request will not be replayed. status / resync");
        }
        for (unsigned i = 0; i < 64; ++i) {
            int c = getchar_timeout_us(0);
            if (c == PICO_ERROR_TIMEOUT) break;
            if (c == '\r' || c == '\n') {
                if (command_overflow) puts("command too long; discarded");
                else if (command_len) { command[command_len] = 0; execute(); }
                command_len = 0; command_overflow = false;
            } else if (command_len + 1 < sizeof command) command[command_len++] = (char)c;
            else command_overflow = true;
        }
        gpio_put(PICO_DEFAULT_LED_PIN, fault ? (now / 100) % 2 :
                 last_ok ? 1 : (now / 500) % 2);
    }
}
