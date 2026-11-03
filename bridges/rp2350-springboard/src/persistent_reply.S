/* From mozzwald/fujinet-nio experiment/dual-cs-write-capture (Experiment 2 persistent
 * Core1 reply loop), bridges/rp2350-springboard/src. Hardware-proven on a Visor Prism;
 * the two changes from his version are marked below.
 *
 * Separate persistent Core1 experiment. Same RX-first SRAM lookup and PIO
 * ownership protocol; save ABI registers once per arm, not once per request.
 * r12: 0=no pending completion, 1=enabled, 2=cancelled. Every consumed token
 * is validated before another RX request. Count it after the next TX or on
 * stop/fault, and flush it before consuming another completion. */
.syntax unified
.thumb
/* Changed: SCRATCH_X, a bank Core0 never uses, so Core0's SRAM traffic
 * cannot stall these instruction fetches. */
.section .scratch_x.sb_persistent_reply,"ax",%progbits
.p2align 2
.global sb_persistent_reply
.type sb_persistent_reply,%function
.thumb_func
sb_persistent_reply:
    push {r4-r11, lr}
    ldr r3, [r2, #0]
    ldr r4, [r2, #4]
    ldr r2, [r2, #8]
    ldr r5, =0x1f700000
    ldr r6, =0x2000000f
    ldr r7, =0x2000000a
    mov r12, #0
    mov lr, #0                 /* request total; return address is on stack */
.Lnext_request:
    ldrb r11, [r2]
    cmp r11, #0
    bne .Lexit
.Lwait_request:
    ldr r11, [r3, #4]
    tst r11, #0x100
    beq .Lrequest
    ldrb r11, [r2]
    cmp r11, #0
    beq .Lwait_request
    b .Lexit
.Lrequest:
    /* Preserve the baseline's adjacent high-FIFO/low-SIO captures and lookup. */
    ldr r8, [r3, #0x20]
    ldr r9, [r4]
    and r11, r8, r6
    cmp r11, r7
    bne .Lcs1_snapshot
    tst r8, r5
    itee ne
    movwne r10, #0xffff
    ubfxeq r11, r9, #1, #15
    ldrheq r10, [r1, r11, lsl #1]
    str r10, [r3, #0x10]
.Lposted:
    /* All accounting is after publication. Hold requests in LR. During
     * service totals.posted is an invalid-snapshot count, converted on exit. */
    add lr, lr, #1

.macro flush_pending
    cmp r12, #0
    beq .Lflushed\@
    cmp r12, #1
    ite eq
    addeq r9, r0, #12
    addne r9, r0, #16
    ldr r8, [r9]
    adds r8, #1
    str r8, [r9]
    mov r12, #0
.Lflushed\@:
.endm
    flush_pending
.Lwait_completion:
    /* Never treat this RX item as another request, even during cancellation. */
    ldrb r11, [r2]
    cmp r11, #0
    bne .Lexit
    ldr r11, [r3, #4]
    tst r11, #0x100
    bne .Lwait_completion
    ldr r12, [r3, #0x20]
    cmp r12, #0
    beq .Lcompletion_ok
    cmn r12, #1
    bne .Lframing_fault
.Lcompletion_ok:
    add r12, r12, #2          /* FFFFFFFF -> 1; 0 -> 2 */
    b .Lnext_request
/* The selected region is derived only from the owned FIFO snapshot. CS1
 * reads the register table that follows the 64 KiB ROM table (changed;
 * Experiment 2 returned a constant here). A16-A23 alias on CS1. */
.Lcs1_snapshot:
    add r11, r11, #1
    cmp r11, r7
    bne .Linvalid_snapshot
    ubfx r11, r9, #1, #15
    orr r11, r11, #0x8000
    ldrh r10, [r1, r11, lsl #1]
    str r10, [r3, #0x10]
    b .Lposted
.Linvalid_snapshot:
    /* Exactly one old-owned dummy unblocks PULL, with sticky cancellation. */
    movs r10, #1
    str r10, [r3, #0x34]
    movs r10, #0
    str r10, [r3, #0x10]
    ldr r8, [r0, #4]
    adds r8, #1
    str r8, [r0, #4]
    b .Lposted
.Lframing_fault:
    mov r12, #0             /* An invalid token is not a completion outcome. */
    movs r8, #1
    str r8, [r0, #20]
.Lexit:
    flush_pending
    str lr, [r0, #0]
    ldr r8, [r0, #4]
    sub r8, lr, r8
    str r8, [r0, #4]
    ldr r8, [r0, #12]
    ldr r9, [r0, #16]
    add r8, r8, r9
    str r8, [r0, #8]
    pop {r4-r11, pc}
.size sb_persistent_reply, .-sb_persistent_reply
.ltorg
/* Keep the ROM byte buffer and native-word table at the October25 baseline
 * SRAM addresses in this controlled build. No executed instruction or delay. */
.balign 4
.space 16, 0
.section .note.GNU-stack,"",%progbits
