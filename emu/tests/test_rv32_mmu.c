#include <assert.h>

#define main rv32_unused_main
#include "../rv32.c"
#undef main

#define ROOT_PA 0x80100000u
#define L2_PA   0x80200000u

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

static int xlate(uint32_t va, enum MemAccess access, uint32_t *pa)
{
    mem_fault = 0;
    mmu_fault_cause = 0;
    return translate_addr(va, access, pa);
}

int main(void)
{
    uint32_t pa;
    uint32_t leaf;
    uint32_t l2_index0 = L2_PA + 0u * 4u;
    uint32_t l2_index1 = L2_PA + 1u * 4u;
    uint32_t l2_index2 = L2_PA + 2u * 4u;

    ram = calloc(1, RAM_SIZE);
    assert(ram != NULL);

    satp = 0;
    priv = 1;
    mstatus = 0;
    assert(xlate(0x80400000u, ACCESS_FETCH, &pa) && pa == 0x80400000u);
    assert(xlate(0x10000000u, ACCESS_LOAD, &pa) && pa == 0x10000000u);

    put32(ROOT_PA + 0x201u * 4u, (0x80200u << 10) | PTE_V);
    leaf = (0x80400u << 10) | PTE_V | PTE_R | PTE_W | PTE_X | PTE_U;
    put32(l2_index0, leaf);
    satp = SATP_MODE_SV32 | (0x80100u & SATP_PPN_MASK);

    priv = 0;
    assert(xlate(0x80400123u, ACCESS_LOAD, &pa) && pa == 0x80400123u);
    assert(xlate(0x80400123u, ACCESS_FETCH, &pa) && pa == 0x80400123u);
    assert(xlate(0x80400123u, ACCESS_STORE, &pa) && pa == 0x80400123u);

    priv = 1;
    assert(!xlate(0x80400123u, ACCESS_LOAD, &pa));
    assert(mmu_fault_cause == CAUSE_LOAD_PF);
    mstatus |= MSTATUS_SUM;
    assert(xlate(0x80400123u, ACCESS_LOAD, &pa));
    mstatus &= ~MSTATUS_SUM;

    put32(ROOT_PA + 0x202u * 4u, (0x80b00u << 10) | PTE_V |
          PTE_R | PTE_X | PTE_A);
    assert(xlate(0x80b00123u, ACCESS_FETCH, &pa) && pa == 0x80b00123u);

    put32(l2_index1, (0x80400u << 10) | PTE_V | PTE_R | PTE_X | PTE_U);
    priv = 0;
    assert(xlate(0x80401123u, ACCESS_LOAD, &pa));
    assert(!xlate(0x80401123u, ACCESS_STORE, &pa));
    assert(mmu_fault_cause == CAUSE_STORE_PF);

    put32(l2_index2, (0x80400u << 10) | PTE_V | PTE_X | PTE_U);
    priv = 0;
    assert(!xlate(0x80402123u, ACCESS_LOAD, &pa));
    mstatus |= MSTATUS_MXR;
    assert(xlate(0x80402123u, ACCESS_LOAD, &pa));
    mstatus &= ~MSTATUS_MXR;

    put32(l2_index1, 0);
    priv = 1;
    assert(!xlate(0x80401123u, ACCESS_LOAD, &pa));
    assert(mmu_fault_cause == CAUSE_LOAD_PF);

    assert(!xlate(0x90000000u, ACCESS_FETCH, &pa));
    assert(mmu_fault_cause == CAUSE_INST_PF);

    priv = 0;
    put32(l2_index0, (0x80400u << 10) | PTE_V | PTE_R | PTE_W | PTE_X | PTE_U);
    assert(xlate(0x80400123u, ACCESS_LOAD, &pa));
    assert((get32(l2_index0) & PTE_A) != 0);
    assert(xlate(0x80400123u, ACCESS_STORE, &pa));
    assert((get32(l2_index0) & PTE_D) != 0);

    csr_write(0x180, 0xc0000000u);
    assert(satp == (SATP_MODE_SV32 | (0x80100u & SATP_PPN_MASK)));
    csr_write(0x180, 0u);
    assert(satp == 0);
    assert(xlate(0x80400123u, ACCESS_LOAD, &pa) && pa == 0x80400123u);

    free(ram);
    return 0;
}
