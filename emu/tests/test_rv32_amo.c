#include <assert.h>

#define main rv32_unused_main
#include "../rv32.c"
#undef main

#define TEST_PC 0x80010000u
#define VEC     0x80020000u
#define ADDR1   0x80400000u
#define ADDR2   0x80401000u

static void put32(uint32_t pa, uint32_t val)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        ram[pa - RAM_BASE + i] = (uint8_t)(val >> (8 * i));
}

static uint32_t get32(uint32_t pa)
{
    return (uint32_t)ram[pa - RAM_BASE] |
           ((uint32_t)ram[pa - RAM_BASE + 1] << 8) |
           ((uint32_t)ram[pa - RAM_BASE + 2] << 16) |
           ((uint32_t)ram[pa - RAM_BASE + 3] << 24);
}

static void reset_cpu(void)
{
    unsigned i;

    for (i = 0; i < 32; i++)
        regs[i] = 0;
    satp = 0;
    priv = 1;
    mstatus = 0;
    stvec = VEC;
    pc = 0;
    scause = 0;
    stval = 0;
    sepc = 0;
    reservation = ~0u;
    mem_fault = 0;
    mmu_fault_cause = 0;
    put32(ADDR1, 0x11223344u);
    put32(ADDR2, 0x55667788u);
}

static uint32_t atomic(uint32_t funct5, uint32_t aqrl, unsigned rs2,
                       unsigned rs1, unsigned rd)
{
    return (funct5 << 27) | (aqrl << 25) | (rs2 << 20) | (rs1 << 15) |
           (2u << 12) | (rd << 7) | 0x2fu;
}

static void expect_trap(uint32_t inst, uint32_t cause, uint32_t tval)
{
    execute(inst, TEST_PC);
    assert(scause == cause);
    assert(stval == tval);
    assert(pc == VEC);
}

int main(void)
{
    uint32_t inst;

    ram = calloc(1, RAM_SIZE);
    assert(ram != NULL);

    reset_cpu();
    regs[1] = ADDR1;
    regs[2] = 3;
    execute(atomic(0x00u, 0, 2, 1, 10), TEST_PC);
    assert(scause == 0);
    assert(regs[10] == 0x11223344u);
    assert(get32(ADDR1) == 0x11223347u);

    reset_cpu();
    put32(ADDR1, 8);
    regs[1] = ADDR1;
    execute(atomic(0x02u, 0, 0, 1, 10), TEST_PC);
    assert(scause == 0);
    assert(regs[10] == 8);
    regs[2] = 11;
    execute(atomic(0x03u, 0, 2, 1, 11), TEST_PC);
    assert(scause == 0);
    assert(regs[11] == 0);
    assert(get32(ADDR1) == 11);

    reset_cpu();
    regs[1] = ADDR1;
    execute(atomic(0x02u, 0, 0, 1, 10), TEST_PC);
    assert(scause == 0);
    regs[1] = ADDR2;
    regs[2] = 1;
    execute(atomic(0x00u, 0, 2, 1, 12), TEST_PC);
    assert(scause == 0);
    regs[1] = ADDR1;
    regs[2] = 99;
    execute(atomic(0x03u, 0, 2, 1, 11), TEST_PC);
    assert(scause == 0);
    assert(regs[11] == 1);
    assert(get32(ADDR1) == 0x11223344u);

    reset_cpu();
    regs[1] = ADDR1;
    execute(atomic(0x02u, 0, 0, 1, 10), TEST_PC);
    assert(scause == 0);
    regs[1] = ADDR2;
    regs[2] = 7;
    execute((2u << 20) | (1u << 15) | (2u << 12) | 0x23u, TEST_PC);
    assert(scause == 0);
    regs[1] = ADDR1;
    regs[2] = 99;
    execute(atomic(0x03u, 0, 2, 1, 11), TEST_PC);
    assert(regs[11] == 1);
    assert(get32(ADDR1) == 0x11223344u);

    reset_cpu();
    regs[1] = ADDR1 + 1u;
    inst = atomic(0x02u, 0, 0, 1, 10);
    expect_trap(inst, 4, ADDR1 + 1u);

    reset_cpu();
    regs[1] = ADDR1 + 1u;
    regs[2] = 0xdeadbeefu;
    inst = atomic(0x03u, 0, 2, 1, 10);
    expect_trap(inst, 4, ADDR1 + 1u);
    assert(get32(ADDR1) == 0x11223344u);

    reset_cpu();
    regs[1] = ADDR1 + 2u;
    regs[2] = 1;
    inst = atomic(0x00u, 0, 2, 1, 10);
    expect_trap(inst, 4, ADDR1 + 2u);
    assert(get32(ADDR1) == 0x11223344u);

    reset_cpu();
    regs[1] = ADDR1;
    inst = atomic(0x02u, 1u, 0, 1, 10);
    expect_trap(inst, CAUSE_ILLEGAL, inst);

    reset_cpu();
    regs[1] = ADDR1;
    regs[2] = 1;
    inst = atomic(0x03u, 2u, 2, 1, 10);
    expect_trap(inst, CAUSE_ILLEGAL, inst);

    reset_cpu();
    regs[1] = ADDR1;
    execute(atomic(0x02u, 2u, 0, 1, 10), TEST_PC);
    assert(scause == 0);
    regs[2] = 1;
    execute(atomic(0x03u, 1u, 2, 1, 10), TEST_PC);
    assert(scause == 0);
    execute(atomic(0x02u, 3u, 0, 1, 10), TEST_PC);
    assert(scause == 0);
    execute(atomic(0x03u, 3u, 2, 1, 10), TEST_PC);
    assert(scause == 0);

    reset_cpu();
    execute(0x0ff0000fu, TEST_PC);
    assert(scause == 0);
    execute(0x0000100fu, TEST_PC);
    assert(scause == 0);

    free(ram);
    return 0;
}
