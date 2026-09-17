#include "stimulus.h"
#include <string.h>
void stimulus_init(stimulus_control *s, void (*start)(void *),
                   void (*release)(void *), void *ctx) {
    *s = (stimulus_control){.start = start,
                            .release = release,
                            .ctx = ctx,
                            .result = "idle generated=0"};
    release(ctx);
}
static void finish(stimulus_control *s, const char *result) {
    s->release(s->ctx);
    s->active = false;
    s->result = result;
}
void stimulus_poll(stimulus_control *s, uint64_t now, bool connected,
                   bool done) {
    if (!connected) {
        s->used = 0;
        s->overflow = false;
        if (s->active)
            finish(s, "aborted disconnect generated=unknown");
    } else if (s->active) {
        if (done) {
            s->generated = 16;
            finish(s, "complete generated=16 nominal_hz=100000");
        } else if (now >= s->deadline)
            finish(s, "aborted timeout generated=unknown");
    }
}
const char *stimulus_feed(stimulus_control *s, int ch, uint64_t now) {
    if (ch != '\n' && ch != '\r') {
        if (ch < 32 || ch > 126 || s->used >= sizeof(s->line) - 1)
            s->overflow = true;
        else if (!s->overflow)
            s->line[s->used++] = (char)ch;
        return NULL;
    }
    s->line[s->used] = 0;
    bool invalid = s->overflow;
    unsigned used = s->used;
    s->used = 0;
    s->overflow = false;
    if (invalid)
        return "error invalid or oversized command";
    if (!used)
        return NULL;
    if (!strcmp(s->line, "help"))
        return "commands: run (16 samples), stop, status, help";
    if (!strcmp(s->line, "status"))
        return s->result;
    if (!strcmp(s->line, "stop")) {
        if (s->active)
            finish(s, "stopped generated=unknown");
        return s->result;
    }
    if (!strcmp(s->line, "run")) {
        if (s->active)
            return "error busy";
        s->generated = 0;
        s->active = true;
        s->deadline = now + 100000;
        s->result = "running samples=16 nominal_hz=100000";
        s->start(s->ctx);
        return s->result;
    }
    return "error unknown command";
}

void stimulus_console_step(stimulus_console *c, stimulus_control *s,
                           const stimulus_console_io *io, uint64_t now,
                           bool connected, uint32_t epoch, bool done) {
    const char *before = s->result;
    bool changed = connected != c->connected || epoch != c->epoch;
    if (!connected || changed) {
        /* Even an observed down/up pair with a final high DTR is a new session.
         * Reset the parser, abort an old run, and discard queued old requests
         * BEFORE announcing readiness. */
        stimulus_poll(s, now, false, false);
        io->flush(io->ctx);
        c->ready = false;
    }
    c->connected = connected;
    c->epoch = epoch;
    stimulus_poll(s, now, connected, done);
    if (!connected)
        return;
    if (!c->ready) {
        if (!io->try_reply(io->ctx, s->result))
            return;
        c->ready = true;
    } else if (before != s->result) {
        (void)io->try_reply(io->ctx, s->result);
    }
    /* One byte and at most two nonblocking replies per iteration. */
    int ch = io->read_byte(io->ctx);
    if (ch >= 0) {
        const char *result = stimulus_feed(s, ch, now);
        if (result)
            (void)io->try_reply(io->ctx, result);
    }
}
