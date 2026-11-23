#include <assert.h>
#include <math.h>
#include <string.h>

#define main rv32_unused_main
#include "../rv32.c"
#undef main

#define TEST_PC 0x80010000u
#define VEC     0x80020000u
#define ADDR1   0x80400000u

static void put32(uint32_t pa, uint32_t val)
{
    unsigned i;

    for (i = 0; i < 4; i++)
        ram[pa - RAM_BASE + i] = (uint8_t)(val >> (8 * i));
}

static void put64(uint32_t pa, uint64_t val)
{
    put32(pa, (uint32_t)val);
    put32(pa + 4, (uint32_t)(val >> 32));
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

    for (i = 0; i < 32; i++) {
        regs[i] = 0;
        fpr[i] = 0;
    }
    satp = 0;
    priv = 1;
    mstatus = 0;
    sstatus = 0;
    stvec = VEC;
    fcsr = 0;
    pc = 0;
    scause = 0;
    stval = 0;
    sepc = 0;
    mem_fault = 0;
    mmu_fault_cause = 0;
}

static void run(uint32_t inst)
{
    pc = TEST_PC + 4;
    execute(inst, TEST_PC);
}

static uint32_t opfp(uint32_t funct7, uint32_t rs2, uint32_t rs1,
                     uint32_t funct3, uint32_t rd)
{
    return insn_r(funct7, rs2, rs1, funct3, rd, 0x53u);
}

static void test_fp_load_store(void)
{
    reset_cpu();
    regs[1] = ADDR1;
    put32(ADDR1 + 4, f32_bits(1.5f));
    run(insn_i(4, 1, 2, 8, 0x07u));
    assert(scause == 0);
    assert((uint32_t)fpr[8] == f32_bits(1.5f));
    assert(mstatus & MSTATUS_FS);

    reset_cpu();
    regs[1] = ADDR1;
    fpr[9] = f32_bits(-2.25f);
    run(insn_s_fp(8, 9, 1, 2));
    assert(scause == 0);
    assert(get32(ADDR1 + 8) == f32_bits(-2.25f));

    reset_cpu();
    regs[1] = ADDR1;
    fpr[8] = f64_bits(3.5);
    run(insn_s_fp(16, 8, 1, 3));
    assert(scause == 0);
    run(insn_i(16, 1, 3, 10, 0x07u));
    assert(scause == 0);
    assert(fpr[10] == f64_bits(3.5));
}

static void test_fp_arithmetic(void)
{
    reset_cpu();
    fpr[8] = f32_bits(1.5f);
    fpr[9] = f32_bits(2.0f);
    run(opfp(0x00, 9, 8, 0, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == f32_bits(3.5f));

    reset_cpu();
    fpr[8] = f64_bits(5.0);
    fpr[9] = f64_bits(2.0);
    run(opfp(0x0d, 9, 8, 0, 10));
    assert(scause == 0);
    assert(fpr[10] == f64_bits(2.5));

    reset_cpu();
    fpr[8] = f32_bits(9.0f);
    run(opfp(0x2c, 0, 8, 0, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == f32_bits(3.0f));

    reset_cpu();
    fpr[8] = f32_bits(2.0f);
    fpr[9] = f32_bits(3.0f);
    fpr[10] = f32_bits(5.0f);
    run(0x43u | (4u << 7) | (8u << 15) | (9u << 20) | (10u << 27));
    assert(scause == 0);
    assert((uint32_t)fpr[4] == f32_bits(11.0f));
}

static void test_fp_subnormal(void)
{
    reset_cpu();
    fpr[8] = 0x007fffffu;
    fpr[9] = f32_bits(1.0f);
    run(opfp(0x08, 9, 8, 0, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == 0x007fffffu);
    assert(fcsr == 0);

    reset_cpu();
    fpr[8] = 0x00000001u;
    fpr[9] = 0x00000001u;
    run(opfp(0x00, 9, 8, 0, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == 0x00000002u);
    assert(fcsr == 0);

    reset_cpu();
    fpr[8] = 1;
    fpr[9] = f64_bits(1.0);
    run(opfp(0x09, 9, 8, 0, 10));
    assert(scause == 0);
    assert(fpr[10] == 1);
    assert(fcsr == 0);
}

static void test_fp_sign_minmax(void)
{
    reset_cpu();
    fpr[8] = f32_bits(1.5f);
    fpr[9] = 0x80000000u;
    run(opfp(0x10, 9, 8, 0, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == f32_bits(-1.5f));

    reset_cpu();
    fpr[8] = 0x80000000u;
    fpr[9] = 0;
    run(opfp(0x14, 9, 8, 0, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == 0x80000000u);

    reset_cpu();
    fpr[8] = 0x80000000u;
    fpr[9] = 0;
    run(opfp(0x14, 9, 8, 1, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == 0);
}

static void test_fp_convert(void)
{
    reset_cpu();
    regs[9] = 42;
    run(opfp(0x68, 0, 9, 0, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == f32_bits(42.0f));

    reset_cpu();
    regs[9] = 0xfffffffeu;
    run(opfp(0x68, 1, 9, 0, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == f32_bits(4294967294.0f));

    reset_cpu();
    fpr[9] = f32_bits(3.75f);
    run(opfp(0x60, 0, 9, 1, 10));
    assert(scause == 0);
    assert(regs[10] == 3);

    reset_cpu();
    fpr[9] = f64_bits(-3.75);
    run(opfp(0x61, 0, 9, 0, 10));
    assert(scause == 0);
    assert(regs[10] == (uint32_t)-4);

    reset_cpu();
    fpr[9] = f32_bits(INFINITY);
    run(opfp(0x60, 0, 9, 0, 10));
    assert(scause == 0);
    assert(regs[10] == 0x7fffffffu);
    assert(fcsr & 0x10u);

    reset_cpu();
    fpr[9] = f64_bits(1.25);
    run(opfp(0x20, 1, 9, 0, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == f32_bits(1.25f));

    reset_cpu();
    fpr[9] = f32_bits(1.25f);
    run(opfp(0x21, 0, 9, 0, 10));
    assert(scause == 0);
    assert(fpr[10] == f64_bits(1.25));
}

static void test_fp_compare_class(void)
{
    reset_cpu();
    fpr[8] = f32_bits(1.5f);
    fpr[9] = f32_bits(1.5f);
    run(opfp(0x50, 9, 8, 2, 10));
    assert(scause == 0);
    assert(regs[10] == 1);

    reset_cpu();
    fpr[8] = 0x7f800001u;
    fpr[9] = f32_bits(1.0f);
    run(opfp(0x50, 9, 8, 1, 10));
    assert(scause == 0);
    assert(regs[10] == 0);
    assert(fcsr & 0x10u);

    reset_cpu();
    fpr[8] = 0x80000000u;
    run(opfp(0x70, 0, 8, 1, 10));
    assert(scause == 0);
    assert(regs[10] == 1u << 3);

    reset_cpu();
    regs[9] = 0x40490fdbu;
    run(opfp(0x78, 0, 9, 0, 10));
    assert(scause == 0);
    assert((uint32_t)fpr[10] == 0x40490fdbu);

    reset_cpu();
    fpr[9] = 0x40490fdbu;
    run(opfp(0x70, 0, 9, 0, 10));
    assert(scause == 0);
    assert(regs[10] == 0x40490fdbu);
}

static void test_fp_csr_and_illegal(void)
{
    reset_cpu();
    csr_write(0x003, 0x1fu | (3u << 5));
    assert(csr_read(0x001) == 0x1fu);
    assert(csr_read(0x002) == 3u);
    assert(csr_read(0x003) == 0x7fu);
    assert((csr_read(0x301) & 0x28u) == 0x28u);

    reset_cpu();
    fpr[8] = f32_bits(16777217.0f);
    run(opfp(0x68, 0, 8, 5, 10));
    assert(scause == CAUSE_ILLEGAL);
    assert(scause != 0);

    reset_cpu();
    run(opfp(0x2c, 1, 8, 0, 10));
    assert(scause == CAUSE_ILLEGAL);

    reset_cpu();
    run(opfp(0x50, 8, 8, 3, 10));
    assert(scause == CAUSE_ILLEGAL);
}

static void test_compressed_fp(void)
{
    uint32_t inst;

    reset_cpu();
    regs[8] = ADDR1;
    put32(ADDR1 + 4, f32_bits(2.5f));
    assert(rv32c_expand(0x2040, &inst));
    run(inst);
    assert(scause == 0);
    assert((uint32_t)fpr[8] == f32_bits(2.5f));

    reset_cpu();
    regs[8] = ADDR1;
    fpr[9] = f32_bits(6.5f);
    assert(rv32c_expand(0xe044, &inst));
    run(inst);
    assert(scause == 0);
    assert(get32(ADDR1 + 4) == f32_bits(6.5f));

    reset_cpu();
    regs[2] = ADDR1;
    put32(ADDR1 + 4, f32_bits(3.5f));
    assert(rv32c_expand(0x6412, &inst));
    run(inst);
    assert(scause == 0);
    assert((uint32_t)fpr[8] == f32_bits(3.5f));

    reset_cpu();
    regs[2] = ADDR1;
    fpr[9] = f32_bits(7.5f);
    assert(rv32c_expand(0xe226, &inst));
    run(inst);
    assert(scause == 0);
    assert(get32(ADDR1 + 4) == f32_bits(7.5f));

    reset_cpu();
    regs[2] = ADDR1;
    put64(ADDR1, f64_bits(8.5));
    assert(rv32c_expand(0x2402, &inst));
    run(inst);
    assert(scause == 0);
    assert(fpr[8] == f64_bits(8.5));

    reset_cpu();
    regs[2] = ADDR1;
    put32(ADDR1 + 8, f32_bits(3.5f));
    put32(ADDR1 + 64, f32_bits(4.5f));
    assert(rv32c_expand(0x2422, &inst));
    run(inst);
    assert(scause == 0);
    assert((uint32_t)fpr[8] == f32_bits(3.5f));

    reset_cpu();
    regs[2] = ADDR1;
    put32(ADDR1 + 8, f32_bits(3.5f));
    put32(ADDR1 + 64, f32_bits(4.5f));
    assert(rv32c_expand(0x2406, &inst));
    run(inst);
    assert(scause == 0);
    assert((uint32_t)fpr[8] == f32_bits(4.5f));

    reset_cpu();
    regs[2] = ADDR1;
    fpr[8] = f64_bits(9.5);
    assert(rv32c_expand(0xa022, &inst));
    run(inst);
    assert(scause == 0);
    assert(get32(ADDR1) == (uint32_t)f64_bits(9.5));
    assert(get32(ADDR1 + 4) == (uint32_t)(f64_bits(9.5) >> 32));
}

int main(void)
{
    ram = calloc(1, RAM_SIZE);
    assert(ram != NULL);

    test_fp_load_store();
    test_fp_arithmetic();
    test_fp_subnormal();
    test_fp_sign_minmax();
    test_fp_convert();
    test_fp_compare_class();
    test_fp_csr_and_illegal();
    test_compressed_fp();

    free(ram);
    return 0;
}
