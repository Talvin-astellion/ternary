#include <assert.h>

#define main rv32_unused_main
#include "../rv32.c"
#undef main

#define OLD_PC  0x80030000u
#define VEC     0x80020000u

static void reset_irq(void)
{
    priv = 1;
    pc = OLD_PC;
    sepc = 0;
    scause = 0;
    stval = 0;
    stvec = VEC;
    sstatus = MSTATUS_SIE;
    mstatus = MSTATUS_SIE;
    sie = 0;
    sip = 0;
    timer_deadline = UINT64_MAX;
    instret = 0;
    guest_time = 0;
    time_inst_mode = 1;
    sleep_wfi = 1;
    debug_sbi = 0;
}

static void expect_irq(uint32_t cause, uint32_t vec)
{
    assert(maybe_interrupt() == 1);
    assert(scause == (cause | 0x80000000u));
    assert(stval == 0);
    assert(sepc == OLD_PC);
    assert(pc == vec);
    assert(priv == 1);
    assert((sstatus & MSTATUS_SIE) == 0);
    assert((sstatus & MSTATUS_SPIE) != 0);
    assert((sstatus & MSTATUS_SPP) != 0);
}

int main(void)
{
    ram = calloc(1, RAM_SIZE);
    assert(ram != NULL);

    reset_irq();
    sie = SIE_STIE;
    sip = SIP_STIP;
    expect_irq(5, VEC);
    assert((sip & SIP_STIP) == 0);

    reset_irq();
    csr_write(0x105, VEC | 1u);
    assert(stvec == (VEC | 1u));
    sie = SIE_STIE;
    sip = SIP_STIP;
    expect_irq(5, VEC + 4u * 5u);

    reset_irq();
    csr_write(0x105, VEC | 1u);
    execute(0xffffffffu, OLD_PC);
    assert(scause == CAUSE_ILLEGAL);
    assert(stval == 0xffffffffu);
    assert(pc == VEC);

    reset_irq();
    sie = SIE_STIE | SIE_SSIE;
    sip = SIP_STIP | SIP_SSIP;
    expect_irq(5, VEC);
    assert((sip & SIP_SSIP) != 0);

    reset_irq();
    sie = SIE_SSIE;
    sip = SIP_SSIP;
    expect_irq(1, VEC);
    assert(sip == 0);

    reset_irq();
    sie = SIE_SEIE;
    sip = SIP_SEIP;
    expect_irq(9, VEC);
    assert(sip == 0);

    reset_irq();
    sip = SIP_STIP;
    assert(maybe_interrupt() == 0);
    assert(pc == OLD_PC);
    assert(scause == 0);

    reset_irq();
    sstatus = 0;
    mstatus = 0;
    sie = SIE_STIE;
    sip = SIP_STIP;
    assert(maybe_interrupt() == 0);

    reset_irq();
    sie = SIE_SSIE;
    sip = SIP_SSIP;
    assert(interrupt_pending());
    sstatus &= ~MSTATUS_SIE;
    assert(!interrupt_pending());

    reset_irq();
    instret = 64;
    sie = SIE_STIE;
    timer_deadline = 1;
    assert(interrupt_pending());
    assert((sip & SIP_STIP) != 0);
    assert(timer_deadline == UINT64_MAX);

    reset_irq();
    stvec = VEC;
    do_trap(CAUSE_ILLEGAL, 0xffffffffu, 0, OLD_PC);
    assert((sstatus & MSTATUS_SPP) != 0);
    execute(0x10200073u, OLD_PC + 4u);
    assert(pc == OLD_PC);
    assert(priv == 1);
    assert((sstatus & MSTATUS_SIE) != 0);
    assert((sstatus & MSTATUS_SPP) == 0);

    free(ram);
    return 0;
}
