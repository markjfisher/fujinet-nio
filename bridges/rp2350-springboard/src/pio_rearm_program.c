/* From mozzwald/fujinet-nio experiment/dual-cs-write-capture (Experiment 2 dual-select
 * read owner), bridges/rp2350-springboard/src. Hardware-proven on a Visor Prism. */
#include "pio_rearm_program.h"
#include <apio.h>

void sb_pio_rearm_program_init(bool enable) {
    APIO_ENABLE_GPIOS();APIO_ENABLE_PIOS();
    for(int pin=0;pin<=46;++pin) {
        if(pin==39)continue;
        APIO_GPIO_INPUT_ONLY(pin);APIO_GPIO_PULL_NONE(pin);
    }
    APIO_ASM_INIT();APIO_SET_BLOCK_VAR(1);APIO_GPIOBASE_16();APIO_SET_SM_VAR(0);
    unsigned short program[]={
        APIO_MOV_OSR_PINS,                         /*0 qualify */
        APIO_OUT_X(4),
        APIO_JMP_X_NOT_Y(29),
        APIO_IRQ_CLEAR(0)|(5u<<8),                 /*3 original settle delay */
        APIO_IN_PINS(32),                          /*4 full-word request autopush */
        APIO_PULL_BLOCK,                           /*5 same blocking reply owner */
        APIO_OUT_PINS(16)|(SB_PIO_REPLY_MARKER?0x1800u:0),
        APIO_MOV_OSR_PINS,
        APIO_OUT_X(4),
        APIO_JMP_X_NOT_Y(15),                      /*9 compare original region Y */
        APIO_JMP_PIN(12),                          /*10 RESET released */
        APIO_JMP(15),
        (APIO_MOV_PINDIRS_STATUS|8u)|(SB_PIO_REPLY_MARKER?0:0x1800u),
        APIO_MOV_X_STATUS|8u,
        APIO_JMP(17),
        APIO_SET_X(0),                             /*15 invalid live state */
        APIO_IRQ_SET(0),
        APIO_WAIT_IRQ_HIGH(0),                     /*17 sole ending consumer */
        APIO_MOV_PINDIRS_NULL|0x1000u,
        APIO_IN_X(32),                             /*19 full-word completion autopush */
        APIO_SET_Y(10),                            /*20 restore and wrap0 */
        APIO_JMP_PIN(25),                          /*21 CS0 high: watch CS1 */
        APIO_WAIT_PIN_HIGH(0),                     /*22 CS0 (SM1) / OE (SM2) */
        APIO_IRQ_SET(0),                           /*23 sticky cancellation */
        APIO_MOV_PINDIRS_NULL|0x1000u,              /*24 direct common clamp */
        APIO_WAIT_GPIO_HIGH(1),                    /*25 CS1 ending */
        APIO_JMP(23),
        APIO_WAIT_PIN_LOW(0),                      /*27 RESET */
        APIO_JMP(23),
        APIO_SET_Y(9),                             /*29 fallback */
        APIO_JMP_X_NOT_Y(20),
        APIO_JMP(3),
    };
    _Static_assert(sizeof(program)/sizeof(program[0])==SB_PR_INSTRUCTIONS,"PIO layout");
    APIO_WRAP_BOTTOM();
    for(unsigned i=0;i<SB_PR_INSTRUCTIONS;++i) {
        if(i==20)APIO_WRAP_TOP();
        APIO_ADD_INSTR(program[i]);
    }
    APIO_SM_CLKDIV_SET(1,0);
    APIO_SM_EXECCTRL_SET((1u<<30)|APIO_EXECCTRL_JMP_PIN(29)|APIO_STATUS_SEL_IRQ|APIO_STATUS_N(0));
    APIO_SM_SHIFTCTRL_SET(APIO_AUTOPUSH|APIO_IN_COUNT(30)|APIO_IN_SHIFTDIR_L|APIO_OUT_SHIFTDIR_R);
    APIO_SM_PINCTRL_SET(APIO_IN_BASE(0)|APIO_OUT_BASE(4)|APIO_OUT_COUNT(16)|
        APIO_SET_BASE(23)|APIO_SET_COUNT(1)|APIO_SIDE_SET_BASE(23)|APIO_SIDE_SET_COUNT(2));
    APIO_SM_EXEC_INSTR(APIO_MOV_PINDIRS_NULL);
    APIO_SM_EXEC_INSTR(APIO_SET_Y(10));
    APIO_SM_EXEC_INSTR(APIO_SET_PINS(0));
    APIO_SM_EXEC_INSTR(APIO_SET_PIN_DIRS(1));
    APIO_SM_JMP_TO_START();
    /* One combined CS ending guard, plus independent OE and RESET clamps.
     * Guards share IRQ/clamp words23/24 but cannot enable outputs or pop RX.
     * Their idle high/held-reset clamps settle under unchanged PC3 delays. */
    for(unsigned sm=1;sm<4;++sm) {
        unsigned entry=sm==1?21:sm==2?22:27;
        APIO_SET_BLOCK_FROM_VAR(1,entry);APIO_SET_SM_VAR(sm);
        APIO_WRAP_BOTTOM();APIO_SET_BLOCK_FROM_VAR(1,24);APIO_WRAP_TOP();
        APIO_SET_BLOCK_FROM_VAR(1,sm==1?26:sm==2?24:28);APIO_END();
        APIO_SM_CLKDIV_SET(1,0);
        APIO_SM_EXECCTRL_SET((1u<<30)|APIO_EXECCTRL_JMP_PIN(0));
        APIO_SM_SHIFTCTRL_SET(APIO_IN_COUNT(30));
        APIO_SM_PINCTRL_SET(APIO_IN_BASE(sm==1?0:sm==2?2:29)|
            APIO_OUT_BASE(4)|APIO_OUT_COUNT(16)|APIO_SIDE_SET_BASE(23)|APIO_SIDE_SET_COUNT(2));
        APIO_SM_JMP_TO_START();
    }
    APIO_SET_BLOCK_FROM_VAR(1,SB_PR_INSTRUCTIONS);APIO_END_BLOCK();
    /* WE assertion and symmetric simultaneous-select conflicts only.
     * Neither valid CS0 nor valid CS1 produces work on these input guards. */
    APIO_SET_BLOCK_VAR(0);APIO_GPIOBASE_16();APIO_SET_SM_VAR(0);
    APIO_WRAP_BOTTOM();APIO_ADD_INSTR(APIO_WAIT_PIN_LOW(0)); /*0 WE */
    APIO_ADD_INSTR(APIO_IRQ_SET_NEXT(0));
    APIO_WRAP_TOP();APIO_ADD_INSTR(APIO_WAIT_PIN_HIGH(0));
    APIO_SET_SM_VAR(1);APIO_START();APIO_WRAP_BOTTOM();
    APIO_ADD_INSTR(APIO_WAIT_PIN_LOW(0));                  /*3 one select asserts */
    APIO_ADD_INSTR(APIO_JMP_PIN(6));                       /*4 other inactive */
    APIO_ADD_INSTR(APIO_IRQ_SET_NEXT(0));                  /*5 conflicting selects */
    APIO_WRAP_TOP();APIO_ADD_INSTR(APIO_WAIT_PIN_HIGH(0));
    for(unsigned sm=0;sm<3;++sm) {
        unsigned entry=sm==0?0:3;
        APIO_SET_BLOCK_FROM_VAR(0,entry);APIO_SET_SM_VAR(sm);
        APIO_WRAP_BOTTOM();APIO_SET_BLOCK_FROM_VAR(0,sm==0?2:6);APIO_WRAP_TOP();
        APIO_SM_CLKDIV_SET(1,0);
        APIO_SM_EXECCTRL_SET(sm==0?0:APIO_EXECCTRL_JMP_PIN(sm==1?0:1));
        APIO_SM_SHIFTCTRL_SET(APIO_IN_COUNT(30));
        APIO_SM_PINCTRL_SET(APIO_IN_BASE(sm==0?3:sm==1?1:0));
        APIO_SM_JMP_TO_START();
    }
    APIO_SET_BLOCK_FROM_VAR(0,7);APIO_END_BLOCK();
    for(int pin=20;pin<=35;++pin)APIO_GPIO_INPUT_OUTPUT(pin,1);
    APIO_GPIO_INPUT_OUTPUT(39,1);
    if(enable){APIO_ENABLE_SMS(0,7);APIO_ENABLE_SMS(1,15);}
}
