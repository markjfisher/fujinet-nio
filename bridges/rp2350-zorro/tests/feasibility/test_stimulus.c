#include <epio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stimulus.h"
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%d: %s\n", __LINE__, #e); exit(1); } } while (0)
static unsigned starts, releases;
static void start(void *p) { (void)p; ++starts; }
static void release(void *p) { (void)p; ++releases; }
static const char *command(stimulus_control *s, const char *c) {
    const char *r = NULL;
    while (*c) { const char *next = stimulus_feed(s, *c++, 10); if (next) r = next; }
    return r;
}
static void control_test(void) {
    stimulus_control s;
    stimulus_init(&s, start, release, NULL);
    CHECK(!s.active && starts == 0 && releases == 1);
    CHECK(strstr(command(&s, "status\n"), "idle"));
    CHECK(strstr(command(&s, "help\n"), "run"));
    CHECK(strstr(command(&s, "bogus\n"), "error"));
    CHECK(strstr(command(&s, "run 100\n"), "error"));
    CHECK(strstr(command(&s, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxrun\n"), "error"));
    CHECK(starts == 0);
    CHECK(strstr(command(&s, "run\r\n"), "running"));
    CHECK(s.active && starts == 1);
    CHECK(strstr(command(&s, "run\n"), "busy"));
    CHECK(starts == 1);
    CHECK(strstr(command(&s, "stop\n"), "stopped"));
    CHECK(!s.active && releases == 2);
    command(&s, "run\n"); stimulus_poll(&s, 20, true, true);
    CHECK(!s.active && s.generated == 16 && strstr(s.result, "complete"));
    command(&s, "run\n"); stimulus_poll(&s, 20, false, false);
    CHECK(!s.active && strstr(s.result, "disconnect"));
    stimulus_poll(&s, 30, true, false);
    CHECK(strstr(command(&s, "status\n"), "disconnect"));
    command(&s, "ru"); stimulus_poll(&s, 30, false, false);
    CHECK(strstr(command(&s, "n\n"), "error"));
    command(&s, "run\n"); stimulus_poll(&s, 100009, true, false);
    CHECK(s.active && releases==4);
    stimulus_poll(&s, 100010, true, false);
    CHECK(!s.active && strstr(s.result, "timeout"));
    CHECK(starts == 4 && releases == 5);
    stimulus_poll(&s, 100011, true, true);
    stimulus_poll(&s, 100012, false, false);
    command(&s, "stop\n");
    CHECK(releases==5);
    /* Embedded NUL cannot truncate an invalid line into an accepted command. */
    command(&s, "run"); stimulus_feed(&s, 0, 10);
    CHECK(strstr(command(&s, "\n"), "error"));
}
static epio_t *waveform_init(void) {
    uint16_t words[STIMULUS_WORDS]; stimulus_program(words);
    const uint16_t expected[] = {0xe02f, 0xa909, 0xe900, 0xe801, 0x0041, 0xc000, 0x0006};
    CHECK(memcmp(words, expected, sizeof expected) == 0);
    epio_t *e = epio_init(); CHECK(e);
    stimulus_registers config=stimulus_config(100000);
    CHECK(config.clkdiv==(1u<<16));
    CHECK(config.execctrl==(6u<<12));
    CHECK(config.shiftctrl==0);
    CHECK(config.pinctrl==(2u | (6u<<5) | (4u<<20) | (1u<<26)));
    CHECK(stimulus_config(125000000).clkdiv==(1250u<<16));
    CHECK(stimulus_config(125050000).clkdiv==((1250u<<16)|(128u<<8)));
    epio_sm_reg_t r = {.clkdiv=config.clkdiv, .execctrl=config.execctrl,
        .shiftctrl=config.shiftctrl, .pinctrl=config.pinctrl};
    epio_set_sm_reg(e, 0, 0, &r);
    for (unsigned i=0; i<7; ++i) epio_set_instr(e, 0, i, words[i]);
    for (unsigned p=2; p<=6; ++p) {
        epio_set_gpio_output_control(e, p, 0);
        epio_set_gpio_output(e, p);
        epio_set_gpio_output_level(e, p, p==6);
    }
    epio_enable_sm(e, 0, 0);
    return e;
}
static void waveform_check(epio_t *e) {
    for (unsigned cycle=0; cycle<550; ++cycle) {
        epio_step_cycles(e, 1);
        unsigned data=0;
        for (unsigned p=2; p<=5; ++p) data |= ((epio_read_pin_states(e)>>p)&1u)<<(p-2);
        unsigned sample = cycle ? (cycle-1)/30 : 0;
        if (sample>15) sample=15;
        CHECK(data==sample);
        unsigned phase = cycle ? (cycle-1)%30 : 0;
        CHECK(((epio_read_pin_states(e)>>6)&1u) == !(cycle>=1 && cycle<481 && phase>=10 && phase<20));
        CHECK(epio_peek_block_irq_num(e,0,0)==(cycle>=481));
    }
}
static epio_t *waveform_rearm(epio_t *e) {
    epio_disable_sm(e,0,0);
    epio_set_gpio_output_level(e,6,1);
    CHECK((epio_read_pin_states(e)&0x40)!=0);
    for (unsigned p=2;p<=6;++p) epio_set_gpio_input(e,p);
    epio_clear_block_irq(e,0,0);
    CHECK(epio_peek_block_irq_num(e,0,0)==0);
    /* epio has no public SM restart API. Recreate its SM to model the SDK's
       pio_sm_init PC/delay/register reset; this does not execute SDK cleanup. */
    epio_free(e);
    e=waveform_init();
    CHECK(epio_peek_block_irq_num(e,0,0)==0);
    return e;
}
static void waveform_test(void) {
    epio_t *e=waveform_init();
    waveform_check(e);
    CHECK(epio_peek_block_irq_num(e,0,0)==1);
    e=waveform_rearm(e);
    waveform_check(e); /* Rearm after a completed burst. */
    e=waveform_rearm(e);
    epio_step_cycles(e,15); /* Abort inside the first /AS low interval. */
    CHECK((epio_read_pin_states(e)&0x40)==0);
    e=waveform_rearm(e);
    waveform_check(e); /* Independent full 16-sample oracle after abort. */
    epio_free(e);
}
typedef struct {
    char queue[128]; unsigned head, tail, flushes, reads, replies;
    bool full, send_on_ready;
    char last[128];
} fake_usb;
static void queue(fake_usb *u, const char *s) {
    size_t n=strlen(s); CHECK(u->tail+n<sizeof u->queue);
    memcpy(u->queue+u->tail,s,n); u->tail+=(unsigned)n;
}
static void flush(void *p) {
    fake_usb *u=p; u->head=u->tail=0; ++u->flushes;
}
static int read_byte(void *p) {
    fake_usb *u=p; ++u->reads;
    return u->head<u->tail ? u->queue[u->head++] : -1;
}
static bool try_reply(void *p, const char *s) {
    fake_usb *u=p; ++u->replies;
    if (u->full) return false;
    snprintf(u->last,sizeof u->last,"%s",s);
    if (u->send_on_ready) { u->send_on_ready=false; queue(u,"run\n"); }
    return true;
}
static void console_test(void) {
    starts=releases=0;
    stimulus_control s; stimulus_init(&s,start,release,NULL);
    stimulus_console c={0}; fake_usb u={0};
    stimulus_console_io io={.ctx=&u,.flush=flush,.read_byte=read_byte,.try_reply=try_reply};
    /* Complete stale request must be discarded before the greeting. A client
       reacting immediately to that greeting must keep its NEW command. */
    queue(&u,"run\n"); u.send_on_ready=true;
    stimulus_console_step(&c,&s,&io,0,true,1,false);
    CHECK(c.ready && u.flushes==1 && u.reads==1 && starts==0);
    for (unsigned i=0;i<3;++i) stimulus_console_step(&c,&s,&io,1,true,1,false);
    CHECK(s.active && starts==1);
    /* DTR false+true occurred between polls: level stays true, epoch changes. */
    queue(&u,"run\n");
    stimulus_console_step(&c,&s,&io,2,true,3,false);
    CHECK(!s.active && releases==2 && u.flushes==2);
    for (unsigned i=0;i<8;++i) stimulus_console_step(&c,&s,&io,3,true,3,false);
    CHECK(starts==1 && strstr(s.result,"disconnect"));
    queue(&u,"ru");
    for (unsigned i=0;i<2;++i) stimulus_console_step(&c,&s,&io,4,true,3,false);
    CHECK(s.used==2);
    stimulus_console_step(&c,&s,&io,5,false,4,false);
    queue(&u,"run\n"); u.full=true;
    unsigned reads=u.reads;
    stimulus_console_step(&c,&s,&io,6,true,5,false);
    CHECK(!c.ready && s.used==0 && u.reads==reads && starts==1);
    /* No busy-loop waiting for a full transmit buffer; each call attempts one
       greeting, with cleanup still polled. Once writable, accept fresh run. */
    unsigned replies=u.replies;
    stimulus_console_step(&c,&s,&io,7,true,5,false);
    CHECK(u.replies==replies+1 && u.reads==reads);
    u.full=false; u.send_on_ready=true;
    for (unsigned i=0;i<4;++i) stimulus_console_step(&c,&s,&io,10,true,5,false);
    CHECK(starts==2 && s.active);
    u.full=true;
    stimulus_console_step(&c,&s,&io,100009,true,5,false);
    CHECK(s.active);
    stimulus_console_step(&c,&s,&io,100010,true,5,false);
    CHECK(!s.active && releases==3 && strstr(s.result,"timeout"));
    stimulus_console_step(&c,&s,&io,100011,true,5,false);
    CHECK(releases==3);
}
int main(void) { control_test(); waveform_test(); console_test(); puts("stimulus: control and 16 exact setup/low/hold samples passed"); }
