/****************************************************************************
 * Copyright (C) 2026 by the TernCpuEmu contributors                          *
 *                                                                          *
 * This file is part of TernCpuEmu.                                         *
 *                                                                          *
 *   TernCpuEmu is free software: you can redistribute it and/or modify it  *
 *   under the terms of the GNU Lesser General Public License as published  *
 *   by the Free Software Foundation, either version 2 of the License, or   *
 *   (at your option) any later version.                                    *
 *                                                                          *
 *   TernCpuEmu is distributed in the hope that it will be useful,          *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 *   GNU Lesser General Public License for more details.                    *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with TernCpuEmu. If not, see                             *
 *   <http://www.gnu.org/licenses/>.                                        *
 ****************************************************************************/

/**
 * @file    rv32.c
 * @brief   RV32IMA guest interpreter for the ternary host emulator.
 *
 * The interpreter is deliberately independent of the ternary ISA: one guest
 * byte occupies one host byte.  The ternary machine is the machine that runs
 * this interpreter, not the machine that runs Linux directly.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RAM_BASE        0x80000000u
#define RAM_SIZE        (128u * 1024u * 1024u)
#define KERNEL_ADDR     0x80400000u
#define DTB_ADDR        0x83e00000u
#define INITRD_ADDR     0x83000000u

#define UART_ADDR       0x10000000u
#define UART_SIZE       0x100u
#define TEST_ADDR       0x00100000u
#define TEST_SIZE       0x1000u

#define MSTATUS_SIE     (1u << 1)
#define MSTATUS_SPIE    (1u << 5)
#define MSTATUS_SPP     (1u << 8)
#define MSTATUS_FS      (3u << 13)
#define MSTATUS_XS      (3u << 15)
#define MSTATUS_SUM     (1u << 18)
#define MSTATUS_MXR     (1u << 19)

#define MSTATUS_MASK    (MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP | \
                         MSTATUS_FS | MSTATUS_XS | MSTATUS_SUM | MSTATUS_MXR)
#define SSTATUS_MASK    MSTATUS_MASK

#define SIE_SSIE        (1u << 1)
#define SIE_STIE        (1u << 5)
#define SIE_SEIE        (1u << 9)

#define SIP_SSIP        (1u << 1)
#define SIP_STIP        (1u << 5)
#define SIP_SEIP        (1u << 9)

#define CAUSE_SOFTWARE  1
#define CAUSE_ILLEGAL   2
#define CAUSE_BREAK     3
#define CAUSE_LOAD      5
#define CAUSE_STORE     7
#define CAUSE_ECALL_U   8
#define CAUSE_ECALL_S   9
#define CAUSE_EXT_S     9
#define CAUSE_TIMER     5

#define SBI_EXT_BASE    0x10u
#define SBI_EXT_TIME    0x54494d45u
#define SBI_EXT_IPI     0x735049u
#define SBI_EXT_RFENCE  0x52464e43u
#define SBI_EXT_HSM     0x48534du
#define SBI_EXT_SRST    0x53525354u
#define SBI_EXT_DBCN    0x4442434eu

#define SBI_SUCCESS     0
#define SBI_ERR_FAILED  (-1)
#define SBI_ERR_NOT_SUPPORTED (-2)
#define SBI_ERR_INVALID_PARAM (-3)

#define SBI_SPEC_VERSION 0x02000000u
#define SBI_IMPL_ID      0
#define SBI_IMPL_VERSION 0

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

#define SATP_MODE_SV32  0x80000000u
#define SATP_ASID_MASK  0x3ff00000u
#define SATP_ASID_SHIFT 20
#define SATP_PPN_MASK   0x003fffffu
#define PTE_V           0x00000001u
#define PTE_R           0x00000002u
#define PTE_W           0x00000004u
#define PTE_X           0x00000008u
#define PTE_U           0x00000010u
#define PTE_A           0x00000040u
#define PTE_D           0x00000080u
#define PTE_RWX         (PTE_R | PTE_W | PTE_X)
#define TLB_SETS        128u
#define TLB_WAYS        2u
#define CAUSE_INST_PF   12
#define CAUSE_LOAD_PF   13
#define CAUSE_STORE_PF  15

enum MemAccess {
    ACCESS_FETCH,
    ACCESS_LOAD,
    ACCESS_STORE
};

static uint8_t *ram;
static uint32_t regs[32];
static uint32_t pc;
static uint32_t sstatus;
static int priv;
static uint32_t sie;
static uint32_t sip;
static uint32_t stvec;
static uint32_t sepc;
static uint32_t scause;
static uint32_t stval;
static uint32_t sscratch;
static uint32_t satp;
static uint32_t mstatus;
static uint32_t mie;
static uint32_t mip;
static uint32_t mtvec;
static uint32_t mepc;
static uint32_t mcause;
static uint32_t mtval;
static uint32_t mscratch;

static uint64_t instret;
static uint64_t cycle;
static uint64_t timer_deadline = UINT64_MAX;
static uint64_t boot_ns;
static uint64_t guest_time;
static uint64_t max_insn;
static uint32_t timebase = 10000000u;
static uint32_t reservation = ~0u;
static int time_inst_mode;
static int sleep_wfi = 1;
static uint64_t trace_count;
static uint64_t sample_count;
static int quiet;
static int debug_sbi;
static int debug_mmu;
static int stats_enabled;
static int mem_fault;
static uint32_t mmu_fault_cause;
static uint64_t mmu_walks;
static uint64_t mmu_faults;
static uint64_t tlb_hits;
static uint64_t tlb_misses;

struct TlbEntry {
    uint32_t vpn;
    uint32_t satp;
    uint32_t base;
    uint32_t pte;
    uint32_t pte_addr;
    uint32_t offset_mask;
    uint8_t valid;
};

static struct TlbEntry tlb[TLB_SETS][TLB_WAYS];
static uint8_t tlb_next_way[TLB_SETS];

static void console_putc(uint8_t c)
{
    ssize_t n;

    do {
        n = write(STDOUT_FILENO, &c, 1);
    } while (n < 0 && errno == EINTR);
}

static void console_write(const uint8_t *p, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(STDOUT_FILENO, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        off += (size_t)n;
    }
}

static ssize_t console_try_read(uint8_t *p, size_t len)
{
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };

    if (len == 0)
        return 0;
    if (poll(&pfd, 1, 0) <= 0)
        return 0;
    return read(STDIN_FILENO, p, len);
}

static void fatal(const char *msg)
{
    fprintf(stderr, "\ntern-linux: %s\n", msg);
    exit(3);
}

static void update_time(void)
{
    struct timespec ts;
    uint64_t ns;

    if (time_inst_mode) {
        guest_time = instret;
        return;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    guest_time = (ns - boot_ns) * timebase / 1000000000ull;
}

static void print_stats(void)
{
    struct timespec ts;
    uint64_t now_ns, wall_ns;

    update_time();
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    wall_ns = now_ns >= boot_ns ? now_ns - boot_ns : 0;
    fprintf(stderr,
            "tern-linux: stats instret=%" PRIu64 " mmu_walks=%" PRIu64
            " mmu_faults=%" PRIu64 " tlb_hits=%" PRIu64
            " tlb_misses=%" PRIu64 " wall_ms=%" PRIu64
            " guest_time=%" PRIu64 "\n",
            instret, mmu_walks, mmu_faults, tlb_hits, tlb_misses,
            (uint64_t)(wall_ns / 1000000ull), guest_time);
}

static inline int in_ram(uint32_t addr)
{
    return addr >= RAM_BASE && (uint64_t)addr + 4 <= (uint64_t)RAM_BASE + RAM_SIZE;
}

static inline int in_uart(uint32_t addr)
{
    return addr >= UART_ADDR && addr < UART_ADDR + UART_SIZE;
}

static inline int in_test(uint32_t addr)
{
    return addr >= TEST_ADDR && addr < TEST_ADDR + TEST_SIZE;
}

static inline int phys_in_ram(uint32_t addr)
{
    return addr >= RAM_BASE && addr < (uint64_t)RAM_BASE + RAM_SIZE;
}

static inline int phys_in_ram_n(uint32_t addr, uint32_t size)
{
    return addr >= RAM_BASE && (uint64_t)addr + size <= (uint64_t)RAM_BASE + RAM_SIZE;
}

static uint8_t phys_load8(uint32_t addr)
{
    if (phys_in_ram(addr))
        return ram[addr - RAM_BASE];
    if (in_uart(addr) || in_test(addr))
        return 0;
    mem_fault = 1;
    return 0;
}

static uint32_t phys_load32(uint32_t addr)
{
    if (!(addr & 3u) && phys_in_ram_n(addr, 4))
        return (uint32_t)ram[addr - RAM_BASE] |
               ((uint32_t)ram[addr - RAM_BASE + 1] << 8) |
               ((uint32_t)ram[addr - RAM_BASE + 2] << 16) |
               ((uint32_t)ram[addr - RAM_BASE + 3] << 24);
    return (uint32_t)phys_load8(addr) |
           ((uint32_t)phys_load8(addr + 1) << 8) |
           ((uint32_t)phys_load8(addr + 2) << 16) |
           ((uint32_t)phys_load8(addr + 3) << 24);
}

static void test_finisher(uint32_t val)
{
    if (val == 0x5555) {
        if (!quiet)
            fprintf(stderr, "\ntern-linux: test finisher PASS\n");
        exit(0);
    }
    if (val == 0x3333 || val == 0x7777) {
        if (!quiet)
            fprintf(stderr, "\ntern-linux: test finisher FAIL (0x%08x)\n", val);
        exit(1);
    }
}

static void phys_store8(uint32_t addr, uint8_t val)
{
    if (phys_in_ram(addr)) {
        ram[addr - RAM_BASE] = val;
        return;
    }
    if (in_uart(addr))
        return;
    if (in_test(addr)) {
        test_finisher(val);
        return;
    }
    mem_fault = 1;
}

static void phys_store32(uint32_t addr, uint32_t val)
{
    if (!(addr & 3u) && phys_in_ram_n(addr, 4)) {
        ram[addr - RAM_BASE] = val & 0xff;
        ram[addr - RAM_BASE + 1] = (val >> 8) & 0xff;
        ram[addr - RAM_BASE + 2] = (val >> 16) & 0xff;
        ram[addr - RAM_BASE + 3] = (val >> 24) & 0xff;
        return;
    }
    if (in_test(addr)) {
        test_finisher(val);
        return;
    }
    phys_store8(addr, val & 0xff);
    phys_store8(addr + 1, (val >> 8) & 0xff);
    phys_store8(addr + 2, (val >> 16) & 0xff);
    phys_store8(addr + 3, (val >> 24) & 0xff);
}

static void mmu_page_fault(uint32_t va, enum MemAccess access,
                           uint32_t pte, uint32_t pte_addr)
{
    mmu_faults++;
    mmu_fault_cause = access == ACCESS_FETCH ? CAUSE_INST_PF :
                      access == ACCESS_STORE ? CAUSE_STORE_PF : CAUSE_LOAD_PF;
    if (debug_mmu) {
        uint32_t root = (satp & SATP_PPN_MASK) << 12;

        fprintf(stderr, "mmu: va=0x%08x access=%u satp=0x%08x pte=0x%08x@0x%08x cause=%u\n",
                va, (unsigned)access, satp, pte, pte_addr, mmu_fault_cause);
        for (uint32_t i = 0; i < 1024u; i++) {
            uint32_t entry = phys_load32(root + i * 4u);
            if (entry & PTE_V)
                fprintf(stderr, "mmu: root[%03x]=0x%08x va=0x%08x\n",
                        i, entry, i << 22);
        }
    }
}

static void tlb_flush(void)
{
    memset(tlb, 0, sizeof(tlb));
    memset(tlb_next_way, 0, sizeof(tlb_next_way));
}

static void tlb_invalidate(uint32_t va, int has_va, uint32_t asid, int has_asid)
{
    unsigned set;
    unsigned way;

    for (set = 0; set < TLB_SETS; set++) {
        for (way = 0; way < TLB_WAYS; way++) {
            struct TlbEntry *e = &tlb[set][way];
            uint32_t base_va;
            uint32_t size;

            if (!e->valid)
                continue;
            if (has_asid &&
                ((e->satp & SATP_ASID_MASK) >> SATP_ASID_SHIFT) != (asid & 0x1ffu))
                continue;
            if (has_va) {
                base_va = e->vpn << 12;
                size = e->offset_mask + 1u;
                if (e->offset_mask == 0x003fffffu)
                    base_va &= ~0x003fffffu;
                if ((uint64_t)va < base_va ||
                    (uint64_t)va >= (uint64_t)base_va + size)
                    continue;
            }
            e->valid = 0;
        }
    }
}

static int pte_allows(uint32_t pte, enum MemAccess access)
{
    uint32_t needed;

    if (priv == 0) {
        if (!(pte & PTE_U))
            return 0;
    } else if (pte & PTE_U) {
        if (access == ACCESS_FETCH)
            return 0;
        if (!(mstatus & MSTATUS_SUM))
            return 0;
    }

    if (access == ACCESS_STORE)
        needed = PTE_W;
    else if (access == ACCESS_FETCH)
        needed = PTE_X;
    else if ((pte & PTE_X) && !(pte & PTE_R) && (mstatus & MSTATUS_MXR))
        needed = PTE_X;
    else
        needed = PTE_R;
    return (pte & needed) != 0;
}

static int tlb_lookup(uint32_t va, enum MemAccess access, uint32_t *pa_out)
{
    unsigned set = (va >> 12) & (TLB_SETS - 1u);
    unsigned way;
    uint32_t vpn = va >> 12;
    uint32_t denied_pte = 0, denied_pte_addr = 0;
    int matched = 0;

    for (way = 0; way < TLB_WAYS; way++) {
        struct TlbEntry *e = &tlb[set][way];

        if (!e->valid || e->vpn != vpn || e->satp != satp)
            continue;
        matched = 1;
        if (!pte_allows(e->pte, access)) {
            denied_pte = e->pte;
            denied_pte_addr = e->pte_addr;
            continue;
        }
        if (!(e->pte & PTE_A) ||
            (access == ACCESS_STORE && !(e->pte & PTE_D))) {
            uint32_t updated = e->pte | PTE_A |
                               (access == ACCESS_STORE ? PTE_D : 0u);

            if (phys_in_ram_n(e->pte_addr, 4))
                phys_store32(e->pte_addr, updated);
            e->pte = updated;
        }
        *pa_out = e->base | (va & e->offset_mask);
        tlb_hits++;
        mmu_fault_cause = 0;
        return 1;
    }
    if (!matched) {
        tlb_misses++;
        return 0;
    }
    tlb_hits++;
    mmu_page_fault(va, access, denied_pte, denied_pte_addr);
    return -1;
}

static void tlb_fill(uint32_t va, uint32_t pte, uint32_t pte_addr,
                     uint32_t base, uint32_t offset_mask)
{
    unsigned set = (va >> 12) & (TLB_SETS - 1u);
    unsigned way = tlb_next_way[set] & (TLB_WAYS - 1u);
    struct TlbEntry *e = &tlb[set][way];

    tlb_next_way[set]++;
    e->vpn = va >> 12;
    e->satp = satp;
    e->base = base;
    e->pte = pte;
    e->pte_addr = pte_addr;
    e->offset_mask = offset_mask;
    e->valid = 1;
}

static int translate_addr(uint32_t va, enum MemAccess access, uint32_t *pa_out)
{
    uint32_t root, pte_addr = 0, pte = 0, base, offset_mask;
    uint32_t vpn0, vpn1;
    int lookup;

    if (!(satp & SATP_MODE_SV32)) {
        *pa_out = va;
        return 1;
    }
    lookup = tlb_lookup(va, access, pa_out);
    if (lookup != 0)
        return lookup > 0;

    mmu_walks++;
    vpn1 = va >> 22;
    vpn0 = (va >> 12) & 0x3ffu;
    root = (satp & SATP_PPN_MASK) << 12;
    pte_addr = root + vpn1 * 4u;
    if (!phys_in_ram_n(pte_addr, 4))
        goto fail;
    pte = phys_load32(pte_addr);
    if (!(pte & PTE_V))
        goto fail;

    if (!(pte & PTE_RWX)) {
        uint32_t next = (pte & 0xfffffc00u) << 2;

        pte_addr = next + vpn0 * 4u;
        if (!phys_in_ram_n(pte_addr, 4))
            goto fail;
        pte = phys_load32(pte_addr);
        if (!(pte & PTE_V) || !(pte & PTE_RWX))
            goto fail;
        if ((pte & PTE_W) && !(pte & PTE_R))
            goto fail;
        base = (pte & 0xfffffc00u) << 2;
        offset_mask = 0xfffu;
    } else {
        if ((pte & PTE_W) && !(pte & PTE_R))
            goto fail;
        if (pte & (0x3ffu << 10))
            goto fail;
        base = (pte & 0xfffffc00u) << 2;
        offset_mask = 0x003fffffu;
    }

    if (!pte_allows(pte, access))
        goto fail;

    if (!(pte & PTE_A) || (access == ACCESS_STORE && !(pte & PTE_D))) {
        uint32_t updated = pte | PTE_A |
                           (access == ACCESS_STORE ? PTE_D : 0u);

        if (phys_in_ram_n(pte_addr, 4))
            phys_store32(pte_addr, updated);
        pte = updated;
    }

    tlb_fill(va, pte, pte_addr, base, offset_mask);
    *pa_out = base | (va & offset_mask);
    mmu_fault_cause = 0;
    return 1;

fail:
    mmu_page_fault(va, access, pte, pte_addr);
    return 0;
}

static uint8_t mem_load8_raw(uint32_t addr)
{
    uint32_t pa;

    if (mem_fault || mmu_fault_cause)
        return 0;
    if (!translate_addr(addr, ACCESS_LOAD, &pa))
        return 0;
    return phys_load8(pa);
}

static uint8_t mem_load8(uint32_t addr)
{
    mem_fault = 0;
    mmu_fault_cause = 0;
    return mem_load8_raw(addr);
}

static uint16_t mem_load16(uint32_t addr)
{
    uint16_t v;

    mem_fault = 0;
    mmu_fault_cause = 0;
    v = mem_load8_raw(addr);
    if (!mem_fault && !mmu_fault_cause)
        v |= (uint16_t)mem_load8_raw(addr + 1) << 8;
    return v;
}

static uint32_t mem_load32(uint32_t addr)
{
    uint32_t pa, v;

    mem_fault = 0;
    mmu_fault_cause = 0;
    if (!(addr & 3u)) {
        if (!translate_addr(addr, ACCESS_LOAD, &pa))
            return 0;
        return phys_load32(pa);
    }
    v = mem_load8_raw(addr);
    if (!mem_fault && !mmu_fault_cause)
        v |= (uint32_t)mem_load8_raw(addr + 1) << 8;
    if (!mem_fault && !mmu_fault_cause)
        v |= (uint32_t)mem_load8_raw(addr + 2) << 16;
    if (!mem_fault && !mmu_fault_cause)
        v |= (uint32_t)mem_load8_raw(addr + 3) << 24;
    return v;
}

static void mem_store8_raw(uint32_t addr, uint8_t val)
{
    uint32_t pa;

    if (mem_fault || mmu_fault_cause)
        return;
    if (!translate_addr(addr, ACCESS_STORE, &pa))
        return;
    phys_store8(pa, val);
}

static void mem_store8(uint32_t addr, uint8_t val)
{
    mem_fault = 0;
    mmu_fault_cause = 0;
    mem_store8_raw(addr, val);
}

static void mem_store16(uint32_t addr, uint16_t val)
{
    mem_fault = 0;
    mmu_fault_cause = 0;
    mem_store8_raw(addr, val & 0xff);
    if (!mem_fault && !mmu_fault_cause)
        mem_store8_raw(addr + 1, val >> 8);
}

static void mem_store32(uint32_t addr, uint32_t val)
{
    uint32_t pa;

    mem_fault = 0;
    mmu_fault_cause = 0;
    if (!(addr & 3u)) {
        if (!translate_addr(addr, ACCESS_STORE, &pa))
            return;
        if (in_test(pa)) {
            test_finisher(val);
            return;
        }
        phys_store32(pa, val);
        return;
    }
    mem_store8_raw(addr, val & 0xff);
    if (!mem_fault && !mmu_fault_cause)
        mem_store8_raw(addr + 1, (val >> 8) & 0xff);
    if (!mem_fault && !mmu_fault_cause)
        mem_store8_raw(addr + 2, (val >> 16) & 0xff);
    if (!mem_fault && !mmu_fault_cause)
        mem_store8_raw(addr + 3, (val >> 24) & 0xff);
}

static int csr_valid(uint32_t csr)
{
    switch (csr) {
    case 0x100: case 0x104: case 0x105: case 0x106: case 0x140:
    case 0x141: case 0x142: case 0x143: case 0x144: case 0x146:
    case 0x180:
    case 0x300: case 0x301: case 0x304: case 0x305: case 0x306:
    case 0x310: case 0x340: case 0x341: case 0x342: case 0x343:
    case 0x344: case 0x346:
    case 0xc00: case 0xc01: case 0xc02: case 0xc80: case 0xc81: case 0xc82:
    case 0xf11: case 0xf12: case 0xf13: case 0xf14:
        return 1;
    default:
        return 0;
    }
}

static int csr_readonly(uint32_t csr)
{
    switch (csr) {
    case 0x142: case 0x143: case 0x301: case 0x342: case 0x343:
    case 0xc00: case 0xc01: case 0xc02: case 0xc80: case 0xc81: case 0xc82:
    case 0xf11: case 0xf12: case 0xf13: case 0xf14:
        return 1;
    default:
        return 0;
    }
}

static int csr_user_ok(uint32_t csr)
{
    switch (csr) {
    case 0xc00: case 0xc01: case 0xc02: case 0xc80: case 0xc81: case 0xc82:
        return 1;
    default:
        return 0;
    }
}

static uint32_t csr_read(uint32_t csr)
{
    switch (csr) {
    case 0x100: return sstatus;
    case 0x104: return sie;
    case 0x105: return stvec;
    case 0x140: return sscratch;
    case 0x141: return sepc;
    case 0x142: return scause;
    case 0x143: return stval;
    case 0x144: return sip;
    case 0x180: return satp;
    case 0x300: return mstatus;
    case 0x301: return 0x40001101u; /* RV32, IMA */
    case 0x304: return mie;
    case 0x305: return mtvec;
    case 0x340: return mscratch;
    case 0x341: return mepc;
    case 0x342: return mcause;
    case 0x343: return mtval;
    case 0x344: return mip;
    case 0xc00: return (uint32_t)cycle;
    case 0xc80: return (uint32_t)(cycle >> 32);
    case 0xc01: update_time(); return (uint32_t)guest_time;
    case 0xc81: update_time(); return (uint32_t)(guest_time >> 32);
    case 0xc02: return (uint32_t)instret;
    case 0xc82: return (uint32_t)(instret >> 32);
    case 0xf11: return 0;
    case 0xf12: return 0;
    case 0xf13: return 0;
    case 0xf14: return 0;
    default: return 0;
    }
}

static void csr_write(uint32_t csr, uint32_t val)
{
    switch (csr) {
    case 0x100:
        sstatus = val & SSTATUS_MASK;
        mstatus = (mstatus & ~SSTATUS_MASK) | sstatus;
        break;
    case 0x104: sie = val & (SIE_SSIE | SIE_STIE | SIE_SEIE); break;
    case 0x105: stvec = val & ~2u; break;
    case 0x140: sscratch = val; break;
    case 0x141: sepc = val & ~3u; break;
    case 0x142: break;
    case 0x143: break;
    case 0x144:
        sip = (sip & SIP_STIP) | (val & ~SIP_STIP);
        break;
    case 0x180: {
        uint32_t old_satp = satp;

        if ((val & 0xc0000000u) == 0)
            satp = 0;
        else if ((val & 0xc0000000u) == SATP_MODE_SV32)
            satp = val & (SATP_MODE_SV32 | SATP_ASID_MASK | SATP_PPN_MASK);
        else if (debug_mmu)
            fprintf(stderr, "mmu: unsupported satp mode 0x%08x ignored\n", val);
        if ((satp ^ old_satp) & ~SATP_ASID_MASK)
            tlb_flush();
        break;
    }
    case 0x300:
        mstatus = val & MSTATUS_MASK;
        sstatus = mstatus & SSTATUS_MASK;
        break;
    case 0x304: mie = val & (SIE_SSIE | SIE_STIE | SIE_SEIE); break;
    case 0x305: mtvec = val & ~2u; break;
    case 0x340: mscratch = val; break;
    case 0x341: mepc = val & ~3u; break;
    case 0x342: break;
    case 0x343: break;
    case 0x344:
        mip = (mip & SIP_STIP) | (val & ~SIP_STIP);
        break;
    default:
        break;
    }
}

static void do_trap(uint32_t cause, uint32_t tval, int interrupt, uint32_t epc)
{
    uint32_t vec;
    uint32_t spp = (priv == 0) ? 0 : MSTATUS_SPP;

    sstatus = (sstatus & ~(MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_SPP)) |
              ((sstatus & MSTATUS_SIE) ? MSTATUS_SPIE : 0) | spp;
    mstatus = (mstatus & ~SSTATUS_MASK) | sstatus;
    priv = 1;
    sepc = epc;
    scause = interrupt ? (cause | 0x80000000u) : cause;
    stval = tval;
    vec = stvec & ~3u;
    if (interrupt && (stvec & 1u))
        vec += 4u * cause;
    pc = vec;
    if (vec == 0)
        fatal("trap before stvec was installed");
}

static int interrupt_pending(void)
{
    if ((instret & 0x3fu) == 0)
        update_time();

    if (timer_deadline != UINT64_MAX && guest_time >= timer_deadline) {
        sip |= SIP_STIP;
        timer_deadline = UINT64_MAX;
    }
    return (sip & sie & (SIP_SSIP | SIP_STIP | SIP_SEIP)) != 0 &&
           (sstatus & MSTATUS_SIE) != 0;
}

static int maybe_interrupt(void)
{
    uint32_t pending;
    uint32_t cause;

    if (!interrupt_pending())
        return 0;

    if ((sip & SIP_SEIP) && (sie & SIE_SEIE)) {
        pending = SIP_SEIP;
        cause = CAUSE_EXT_S;
    } else if ((sip & SIP_STIP) && (sie & SIE_STIE)) {
        pending = SIP_STIP;
        cause = CAUSE_TIMER;
    } else {
        pending = SIP_SSIP;
        cause = CAUSE_SOFTWARE;
    }
    sip &= ~pending;
    if (debug_sbi)
        fprintf(stderr, "irq cause=%u time=%" PRIu64 " pc=0x%08x\n",
                cause, guest_time, pc);
    do_trap(cause, 0, 1, pc);
    return 1;
}

static void set_reg(uint32_t rd, uint32_t val)
{
    if (rd)
        regs[rd] = val;
}

static int32_t imm_i(uint32_t inst)
{
    return (int32_t)inst >> 20;
}

static int32_t imm_s(uint32_t inst)
{
    uint32_t v = ((inst >> 25) << 5) | ((inst >> 7) & 0x1fu);
    return (int32_t)(v << 20) >> 20;
}

static int32_t imm_b(uint32_t inst)
{
    uint32_t v = (((inst >> 31) & 1u) << 12) |
                 (((inst >> 7) & 1u) << 11) |
                 (((inst >> 25) & 0x3fu) << 5) |
                 (((inst >> 8) & 0x0fu) << 1);
    return (int32_t)(v << 19) >> 19;
}

static int32_t imm_j(uint32_t inst)
{
    uint32_t v = (((inst >> 31) & 1u) << 20) |
                 (((inst >> 12) & 0xffu) << 12) |
                 (((inst >> 20) & 1u) << 11) |
                 (((inst >> 21) & 0x3ffu) << 1);
    return (int32_t)(v << 11) >> 11;
}

static uint32_t imm_u(uint32_t inst)
{
    return inst & 0xfffff000u;
}

static uint32_t sbi_ext_supported(uint32_t eid)
{
    switch (eid) {
    case SBI_EXT_BASE:
    case SBI_EXT_TIME:
    case SBI_EXT_IPI:
    case SBI_EXT_RFENCE:
    case SBI_EXT_HSM:
    case SBI_EXT_SRST:
    case SBI_EXT_DBCN:
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
        return 1;
    default:
        return 0;
    }
}

static void sbi_shutdown(uint32_t type, uint32_t reason)
{
    if (!quiet)
        fprintf(stderr, "\ntern-linux: SBI shutdown type=%u reason=%u\n",
                type, reason);
    exit(0);
}

static void sbi_call(void)
{
    uint32_t eid = regs[17];
    uint32_t fid = regs[16];
    uint32_t a0 = regs[10], a1 = regs[11], a2 = regs[12];
    int32_t err = SBI_SUCCESS;
    if (debug_sbi)
        fprintf(stderr, "sbi eid=0x%08x fid=%u a0=0x%08x a1=0x%08x a2=0x%08x pc=0x%08x\n",
                eid, fid, a0, a1, a2, pc);
    uint32_t ret = 0;

    switch (eid) {
    case SBI_EXT_BASE:
        switch (fid) {
        case 0:
            ret = SBI_SPEC_VERSION;
            break;
        case 1:
            ret = SBI_IMPL_ID;
            break;
        case 2:
            ret = SBI_IMPL_VERSION;
            break;
        case 3:
            ret = sbi_ext_supported(a0) ? 1 : 0;
            break;
        case 4:
        case 5:
        case 6:
            ret = 0;
            break;
        default:
            err = SBI_ERR_NOT_SUPPORTED;
            break;
        }
        break;
    case SBI_EXT_TIME:
        if (fid == 0) {
            timer_deadline = ((uint64_t)a1 << 32) | a0;
        } else {
            err = SBI_ERR_NOT_SUPPORTED;
        }
        break;
    case SBI_EXT_IPI:
    case SBI_EXT_RFENCE:
        break;
    case SBI_EXT_HSM:
        switch (fid) {
        case 0:
            err = SBI_ERR_INVALID_PARAM;
            break;
        case 1:
            sbi_shutdown(0, 0);
            break;
        case 2:
            ret = 0;
            break;
        case 3:
            break;
        default:
            err = SBI_ERR_NOT_SUPPORTED;
            break;
        }
        break;
    case SBI_EXT_SRST:
        if (fid == 0)
            sbi_shutdown(a0, a1);
        else
            err = SBI_ERR_NOT_SUPPORTED;
        break;
    case SBI_EXT_DBCN: {
        uint64_t addr = ((uint64_t)a2 << 32) | a1;
        uint32_t i;
        uint8_t buf[256];

        switch (fid) {
        case 0:
            if (a0 > sizeof(buf))
                a0 = sizeof(buf);
            for (i = 0; i < a0; i++) {
                mem_fault = 0;
                buf[i] = phys_load8((uint32_t)(addr + i));
                if (mem_fault)
                    break;
            }
            console_write(buf, i);
            ret = i;
            break;
        case 1: {
            ssize_t n;
            if (a0 > sizeof(buf))
                a0 = sizeof(buf);
            n = console_try_read(buf, a0);
            if (n > 0) {
                for (i = 0; i < (uint32_t)n; i++)
                    phys_store8((uint32_t)(addr + i), buf[i]);
                ret = (uint32_t)n;
            }
            break;
        }
        case 2:
            console_putc((uint8_t)a0);
            break;
        default:
            err = SBI_ERR_NOT_SUPPORTED;
            break;
        }
        break;
    }
    default:
        switch (eid) {
        case 0:
            timer_deadline = a0;
            break;
        case 1:
            console_putc((uint8_t)a0);
            break;
        case 2: {
            uint8_t c;
            ssize_t n = console_try_read(&c, 1);
            ret = n > 0 ? c : (uint32_t)-1;
            break;
        }
        case 3:
        case 4:
        case 5:
        case 6:
            break;
        case 7:
            sbi_shutdown(0, 0);
            break;
        default:
            err = SBI_ERR_NOT_SUPPORTED;
            break;
        }
        break;
    }

    regs[10] = (uint32_t)err;
    regs[11] = ret;
}

static void execute(uint32_t inst, uint32_t cur_pc)
{
    uint32_t opcode = inst & 0x7fu;
    uint32_t rd = (inst >> 7) & 0x1fu;
    uint32_t funct3 = (inst >> 12) & 7u;
    uint32_t rs1 = (inst >> 15) & 0x1fu;
    uint32_t rs2 = (inst >> 20) & 0x1fu;
    uint32_t funct7 = inst >> 25;
    uint32_t addr, val, old;
    int32_t simm;
    int take;

    switch (opcode) {
    case 0x37: /* LUI */
        set_reg(rd, imm_u(inst));
        break;
    case 0x17: /* AUIPC */
        set_reg(rd, cur_pc + imm_u(inst));
        break;
    case 0x6f: /* JAL */
        set_reg(rd, pc);
        pc = cur_pc + (uint32_t)imm_j(inst);
        break;
    case 0x67: /* JALR */
        addr = (regs[rs1] + (uint32_t)imm_i(inst)) & ~1u;
        set_reg(rd, pc);
        pc = addr;
        break;
    case 0x63: /* branches */
        take = 0;
        switch (funct3) {
        case 0: take = regs[rs1] == regs[rs2]; break;
        case 1: take = regs[rs1] != regs[rs2]; break;
        case 4: take = (int32_t)regs[rs1] < (int32_t)regs[rs2]; break;
        case 5: take = (int32_t)regs[rs1] >= (int32_t)regs[rs2]; break;
        case 6: take = regs[rs1] < regs[rs2]; break;
        case 7: take = regs[rs1] >= regs[rs2]; break;
        default:
            do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            return;
        }
        if (take)
            pc = cur_pc + (uint32_t)imm_b(inst);
        break;
    case 0x03: /* loads */
        addr = regs[rs1] + (uint32_t)imm_i(inst);
        mem_fault = 0;
        mmu_fault_cause = 0;
        switch (funct3) {
        case 0:
            val = (uint32_t)(int32_t)(int8_t)mem_load8(addr);
            break;
        case 1:
            val = (uint32_t)(int32_t)(int16_t)mem_load16(addr);
            break;
        case 2:
            val = mem_load32(addr);
            break;
        case 4:
            val = mem_load8(addr);
            break;
        case 5:
            val = mem_load16(addr);
            break;
        default:
            do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            return;
        }
        if (mem_fault || mmu_fault_cause)
            do_trap(mmu_fault_cause ? mmu_fault_cause : CAUSE_LOAD,
                    addr, 0, cur_pc);
        else
            set_reg(rd, val);
        break;
    case 0x23: /* stores */
        addr = regs[rs1] + (uint32_t)imm_s(inst);
        mem_fault = 0;
        mmu_fault_cause = 0;
        switch (funct3) {
        case 0: mem_store8(addr, (uint8_t)regs[rs2]); break;
        case 1: mem_store16(addr, (uint16_t)regs[rs2]); break;
        case 2: mem_store32(addr, regs[rs2]); break;
        default:
            do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            return;
        }
        reservation = ~0u;
        if (mem_fault || mmu_fault_cause)
            do_trap(mmu_fault_cause ? mmu_fault_cause : CAUSE_STORE,
                    addr, 0, cur_pc);
        break;
    case 0x13: /* OP-IMM */
        simm = imm_i(inst);
        switch (funct3) {
        case 0: set_reg(rd, regs[rs1] + (uint32_t)simm); break;
        case 1:
            if (funct7 != 0) {
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
                return;
            }
            set_reg(rd, regs[rs1] << ((inst >> 20) & 0x1fu));
            break;
        case 2:
            set_reg(rd, (int32_t)regs[rs1] < simm ? 1u : 0u);
            break;
        case 3:
            set_reg(rd, regs[rs1] < (uint32_t)simm ? 1u : 0u);
            break;
        case 4: set_reg(rd, regs[rs1] ^ (uint32_t)simm); break;
        case 5:
            if (funct7 == 0)
                set_reg(rd, regs[rs1] >> ((inst >> 20) & 0x1fu));
            else if (funct7 == 0x20)
                set_reg(rd, (uint32_t)((int32_t)regs[rs1] >> ((inst >> 20) & 0x1fu)));
            else
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            break;
        case 6: set_reg(rd, regs[rs1] | (uint32_t)simm); break;
        case 7: set_reg(rd, regs[rs1] & (uint32_t)simm); break;
        }
        break;
    case 0x33: /* OP */
        if (funct7 == 0x01) {
            int32_t sa = (int32_t)regs[rs1], sb = (int32_t)regs[rs2];
            uint64_t ua = regs[rs1], ub = regs[rs2];
            switch (funct3) {
            case 0: set_reg(rd, (uint32_t)((int64_t)sa * sb)); break;
            case 1:
                set_reg(rd, (uint32_t)(((int64_t)sa * sb) >> 32));
                break;
            case 2:
                set_reg(rd, (uint32_t)(((int64_t)sa * (int64_t)ub) >> 32));
                break;
            case 3:
                set_reg(rd, (uint32_t)((ua * ub) >> 32));
                break;
            case 4:
                if (sb == 0)
                    val = 0xffffffffu;
                else if (sa == INT32_MIN && sb == -1)
                    val = (uint32_t)INT32_MIN;
                else
                    val = (uint32_t)(sa / sb);
                set_reg(rd, val);
                break;
            case 5:
                set_reg(rd, sb == 0 ? 0xffffffffu : ua / ub);
                break;
            case 6:
                if (sb == 0)
                    val = (uint32_t)sa;
                else if (sa == INT32_MIN && sb == -1)
                    val = 0;
                else
                    val = (uint32_t)(sa % sb);
                set_reg(rd, val);
                break;
            case 7:
                set_reg(rd, sb == 0 ? ua : ua % ub);
                break;
            }
            break;
        }
        switch (funct3) {
        case 0:
            if (funct7 == 0)
                set_reg(rd, regs[rs1] + regs[rs2]);
            else if (funct7 == 0x20)
                set_reg(rd, regs[rs1] - regs[rs2]);
            else
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            break;
        case 1:
            if (funct7 == 0)
                set_reg(rd, regs[rs1] << (regs[rs2] & 0x1fu));
            else
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            break;
        case 2:
            if (funct7 == 0)
                set_reg(rd, (int32_t)regs[rs1] < (int32_t)regs[rs2] ? 1u : 0u);
            else
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            break;
        case 3:
            if (funct7 == 0)
                set_reg(rd, regs[rs1] < regs[rs2] ? 1u : 0u);
            else
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            break;
        case 4:
            if (funct7 == 0)
                set_reg(rd, regs[rs1] ^ regs[rs2]);
            else
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            break;
        case 5:
            if (funct7 == 0)
                set_reg(rd, regs[rs1] >> (regs[rs2] & 0x1fu));
            else if (funct7 == 0x20)
                set_reg(rd, (uint32_t)((int32_t)regs[rs1] >> (regs[rs2] & 0x1fu)));
            else
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            break;
        case 6:
            if (funct7 == 0)
                set_reg(rd, regs[rs1] | regs[rs2]);
            else
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            break;
        case 7:
            if (funct7 == 0)
                set_reg(rd, regs[rs1] & regs[rs2]);
            else
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            break;
        }
        break;
    case 0x0f: /* FENCE, FENCE.I */
        break;
    case 0x2f: /* A extension */
        if (funct3 != 2) {
            do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
            return;
        }
        addr = regs[rs1];
        mem_fault = 0;
        mmu_fault_cause = 0;
        old = mem_load32(addr);
        if (mem_fault || mmu_fault_cause) {
            do_trap(mmu_fault_cause ? mmu_fault_cause : CAUSE_LOAD,
                    addr, 0, cur_pc);
            return;
        }
        switch (inst >> 27) {
        case 0x02: /* lr.w */
            reservation = addr;
            set_reg(rd, old);
            break;
        case 0x03: /* sc.w */
            if (reservation == addr) {
                mem_fault = 0;
                mmu_fault_cause = 0;
                mem_store32(addr, regs[rs2]);
                if (mem_fault || mmu_fault_cause)
                    do_trap(mmu_fault_cause ? mmu_fault_cause : CAUSE_STORE,
                            addr, 0, cur_pc);
                else
                    set_reg(rd, 0);
            } else {
                set_reg(rd, 1);
            }
            reservation = ~0u;
            break;
        default: {
            uint32_t src = regs[rs2];
            switch (inst >> 27) {
            case 0x00: val = old + src; break;
            case 0x04: val = old ^ src; break;
            case 0x0c: val = old & src; break;
            case 0x08: val = old | src; break;
            case 0x10: val = (int32_t)old < (int32_t)src ? old : src; break;
            case 0x14: val = (int32_t)old > (int32_t)src ? old : src; break;
            case 0x18: val = old < src ? old : src; break;
            case 0x1c: val = old > src ? old : src; break;
            case 0x01: val = src; break;
            default:
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
                return;
            }
            mem_fault = 0;
            mmu_fault_cause = 0;
            mem_store32(addr, val);
            if (mem_fault || mmu_fault_cause)
                do_trap(mmu_fault_cause ? mmu_fault_cause : CAUSE_STORE,
                        addr, 0, cur_pc);
            else
                set_reg(rd, old);
            break;
        }
        }
        break;
    case 0x73: /* SYSTEM */
        if (funct3 == 0) {
            switch (inst) {
            case 0x00000073: /* ecall */
                if (priv == 0)
                    do_trap(CAUSE_ECALL_U, 0, 0, cur_pc);
                else
                    sbi_call();
                break;
            case 0x00100073: /* ebreak */
                do_trap(CAUSE_BREAK, 0, 0, cur_pc);
                break;
            case 0x10200073: /* sret */
                if (priv != 1) {
                    do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
                    break;
                }
                pc = sepc;
                priv = (sstatus & MSTATUS_SPP) ? 1 : 0;
                sstatus = (sstatus & ~MSTATUS_SIE) |
                          ((sstatus & MSTATUS_SPIE) ? MSTATUS_SIE : 0);
                sstatus |= MSTATUS_SPIE;
                sstatus &= ~MSTATUS_SPP;
                mstatus = (mstatus & ~SSTATUS_MASK) | sstatus;
                break;
            case 0x30200073: /* mret */
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
                break;
            case 0x10500073: /* wfi */
                update_time();
                if (sleep_wfi && !interrupt_pending()) {
                    struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000 };
                    nanosleep(&ts, NULL);
                }
                break;
            default:
                if ((inst & 0xfe007fffu) == 0x12000073u) {
                    unsigned rs1 = (inst >> 15) & 0x1fu;
                    unsigned rs2 = (inst >> 20) & 0x1fu;

                    if (priv == 0) {
                        do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
                        break;
                    }
                    tlb_invalidate(rs1 ? regs[rs1] : 0, rs1 != 0,
                                   rs2 ? regs[rs2] : 0, rs2 != 0);
                    break;
                }
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
                break;
            }
            break;
        }
        if (funct3 == 1 || funct3 == 2 || funct3 == 3 ||
            funct3 == 5 || funct3 == 6 || funct3 == 7) {
            uint32_t csr = inst >> 20;
            uint32_t src = (funct3 & 4u) ? rs1 : regs[rs1];
            int write;

            old = csr_read(csr);
            switch (funct3) {
            case 1:
            case 5:
                val = src;
                write = 1;
                break;
            case 2:
            case 6:
                val = old | src;
                write = src != 0;
                break;
            default:
                val = old & ~src;
                write = src != 0;
                break;
            }
            if (!csr_valid(csr) || (write && csr_readonly(csr)) ||
                (priv == 0 && (!csr_user_ok(csr) || write))) {
                do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
                break;
            }
            if (write)
                csr_write(csr, val);
            set_reg(rd, old);
            break;
        }
        do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
        break;
    default:
        do_trap(CAUSE_ILLEGAL, inst, 0, cur_pc);
        break;
    }
}

struct BootImage {
    const char *path;
    uint32_t addr;
    FILE *file;
    size_t size;
};

static int open_boot_image(struct BootImage *image)
{
    long size;

    image->file = fopen(image->path, "rb");
    if (!image->file) {
        fprintf(stderr, "tern-linux: cannot open %s: %s\n", image->path, strerror(errno));
        return -1;
    }
    if (fseek(image->file, 0, SEEK_END) != 0 ||
        (size = ftell(image->file)) < 0 || fseek(image->file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "tern-linux: cannot determine size of %s: %s\n",
                image->path, strerror(errno));
        return -1;
    }
    if (image->addr < RAM_BASE || image->addr >= RAM_BASE + RAM_SIZE ||
        (uint64_t)size > RAM_BASE + RAM_SIZE - image->addr) {
        fprintf(stderr, "tern-linux: %s does not fit in RAM at 0x%08x\n",
                image->path, image->addr);
        return -1;
    }
    image->size = (size_t)size;
    return 0;
}

static int load_boot_images(struct BootImage *images, size_t count)
{
    int result = -1;

    for (size_t i = 0; i < count; i++) {
        struct BootImage *a = &images[i];

        if (open_boot_image(a) != 0)
            goto out;
        for (size_t j = 0; j < i; j++) {
            const struct BootImage *b = &images[j];

            if (a->size && b->size && a->addr < b->addr + b->size &&
                b->addr < a->addr + a->size) {
                fprintf(stderr, "tern-linux: %s at 0x%08x overlaps %s at 0x%08x\n",
                        a->path, a->addr, b->path, b->addr);
                goto out;
            }
        }
    }
    ram = calloc(1, RAM_SIZE);
    if (!ram) {
        fprintf(stderr, "tern-linux: cannot allocate %u bytes of RAM\n", RAM_SIZE);
        goto out;
    }
    for (size_t i = 0; i < count; i++) {
        const struct BootImage *image = &images[i];

        if (fread(ram + (image->addr - RAM_BASE), 1, image->size, image->file) != image->size) {
            fprintf(stderr, "tern-linux: short read of %s\n", image->path);
            free(ram);
            ram = NULL;
            goto out;
        }
    }
    result = 0;
out:
    for (size_t i = 0; i < count; i++) {
        if (images[i].file)
            fclose(images[i].file);
    }
    return result;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static void fdt_patch_u32(uint32_t dtb, size_t dtb_size, const char *prop, uint32_t value)
{
    uint8_t *base = ram + (dtb - RAM_BASE);
    uint32_t total, off_struct, off_strings, pos, end;
    uint32_t len, nameoff;
    const char *name;

    if (dtb_size < 40) {
        if (debug_sbi)
            fprintf(stderr, "fdt: truncated header at 0x%08x\n", dtb);
        return;
    }
    if (be32(base) != FDT_MAGIC) {
        if (debug_sbi)
            fprintf(stderr, "fdt: bad magic 0x%08x\n", be32(base));
        return;
    }
    total = be32(base + 4);
    off_struct = be32(base + 8);
    off_strings = be32(base + 12);
    if (total > dtb_size || off_struct >= total || off_strings >= total)
        return;
    pos = off_struct;
    end = total;
    while (pos + 4 <= end) {
        uint32_t token = be32(base + pos);
        pos += 4;
        if (token == FDT_BEGIN_NODE) {
            while (pos < end && base[pos] != 0)
                pos++;
            pos++;
            pos = (pos + 3u) & ~3u;
        } else if (token == FDT_END_NODE || token == FDT_NOP) {
            continue;
        } else if (token == FDT_PROP) {
            if (pos + 8 > end)
                return;
            len = be32(base + pos);
            nameoff = be32(base + pos + 4);
            pos += 8;
            if (len > end - pos)
                return;
            if (nameoff < end - off_strings &&
                memchr(base + off_strings + nameoff, '\0', end - off_strings - nameoff)) {
                name = (const char *)(base + off_strings + nameoff);
                if (debug_sbi && !strcmp(name, prop))
                    fprintf(stderr, "fdt: patch %s=0x%x\n", prop, value);
                if (len == 4 && !strcmp(name, prop))
                    put_be32(base + pos, value);
            }
            pos = (pos + len + 3u) & ~3u;
        } else if (token == FDT_END) {
            return;
        } else {
            return;
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --kernel Image [options]\n"
            "  --kernel-addr ADDR  load kernel at physical address (default 0x%08x)\n"
            "  --dtb FILE          load DTB at 0x%08x\n"
            "  --initrd FILE       load initramfs at 0x%08x\n"
            "  --dtb-addr ADDR     override the DTB load address\n"
            "  --initrd-addr ADDR  override the initramfs load address\n"
            "  --load ADDR FILE    load raw file at physical address\n"
            "  --pc ADDR           override entry point (default 0x%08x)\n"
            "  --max-insn N        stop after N instructions (0 = none)\n"
            "  --trace N           trace first N instructions\n"
            "  --sample N          print pc/ra every N instructions\n"
            "  --timebase HZ       guest timebase (default %u)\n"
            "  --time-inst         derive guest time from instruction count\n"
             "  --no-sleep          do not sleep in wfi\n"
             "  --debug-sbi         log SBI calls\n"
             "  --debug-mmu         log Sv32 translation faults and satp writes\n"
             "  --stats             print instruction and MMU counters at exit\n"
             "  --quiet             suppress emulator diagnostics\n",
            prog, KERNEL_ADDR, DTB_ADDR, INITRD_ADDR, KERNEL_ADDR, timebase);
}

static int parse_boot_addr(const char *arg, uint32_t *addr)
{
    char *end;
    unsigned long long value;

    errno = 0;
    value = strtoull(arg, &end, 0);
    if (errno || arg[0] < '0' || arg[0] > '9' || *end || value > UINT32_MAX) {
        fprintf(stderr, "tern-linux: invalid address: %s\n", arg);
        return -1;
    }
    *addr = (uint32_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    const char *kernel = NULL;
    const char *dtb = NULL;
    const char *initrd = NULL;
    uint32_t dtb_addr = DTB_ADDR;
    uint32_t initrd_addr = INITRD_ADDR;
    uint32_t kernel_addr = KERNEL_ADDR;
    uint32_t entry = KERNEL_ADDR;
    struct timespec ts;
    struct BootImage *images = calloc((size_t)argc + 3, sizeof(*images));
    size_t image_count = 0, dtb_index = 0, initrd_index = 0;
    int i;

    if (!images) {
        fprintf(stderr, "tern-linux: cannot allocate boot image list\n");
        return 2;
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--kernel") && i + 1 < argc) {
            kernel = argv[++i];
        } else if (!strcmp(argv[i], "--kernel-addr") && i + 1 < argc) {
            if (parse_boot_addr(argv[++i], &kernel_addr) != 0)
                goto invalid_args;
            entry = kernel_addr;
        } else if (!strcmp(argv[i], "--dtb") && i + 1 < argc) {
            dtb = argv[++i];
        } else if (!strcmp(argv[i], "--initrd") && i + 1 < argc) {
            initrd = argv[++i];
        } else if (!strcmp(argv[i], "--load") && i + 2 < argc) {
            uint32_t addr;

            if (parse_boot_addr(argv[++i], &addr) != 0)
                goto invalid_args;
            images[image_count++] = (struct BootImage){ .path = argv[++i], .addr = addr };
        } else if (!strcmp(argv[i], "--pc") && i + 1 < argc) {
            if (parse_boot_addr(argv[++i], &entry) != 0)
                goto invalid_args;
        } else if (!strcmp(argv[i], "--dtb-addr") && i + 1 < argc) {
            if (parse_boot_addr(argv[++i], &dtb_addr) != 0)
                goto invalid_args;
        } else if (!strcmp(argv[i], "--initrd-addr") && i + 1 < argc) {
            if (parse_boot_addr(argv[++i], &initrd_addr) != 0)
                goto invalid_args;
        } else if (!strcmp(argv[i], "--max-insn") && i + 1 < argc) {
            max_insn = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--trace") && i + 1 < argc) {
            trace_count = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--sample") && i + 1 < argc) {
            sample_count = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--timebase") && i + 1 < argc) {
            timebase = (uint32_t)strtoul(argv[++i], NULL, 0);
            if (!timebase)
                timebase = 10000000u;
        } else if (!strcmp(argv[i], "--time-inst")) {
            time_inst_mode = 1;
        } else if (!strcmp(argv[i], "--no-sleep")) {
            sleep_wfi = 0;
        } else if (!strcmp(argv[i], "--debug-sbi")) {
            debug_sbi = 1;
        } else if (!strcmp(argv[i], "--debug-mmu")) {
            debug_mmu = 1;
        } else if (!strcmp(argv[i], "--stats")) {
            stats_enabled = 1;
        } else if (!strcmp(argv[i], "--quiet")) {
            quiet = 1;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            free(images);
            return 0;
        } else {
            fprintf(stderr, "tern-linux: unknown argument %s\n", argv[i]);
            usage(argv[0]);
            goto invalid_args;
        }
    }

    if (!kernel) {
        usage(argv[0]);
        goto invalid_args;
    }

    images[image_count++] = (struct BootImage){ .path = kernel, .addr = kernel_addr };
    if (dtb) {
        dtb_index = image_count;
        images[image_count++] = (struct BootImage){ .path = dtb, .addr = dtb_addr };
    }
    if (initrd) {
        initrd_index = image_count;
        images[image_count++] = (struct BootImage){ .path = initrd, .addr = initrd_addr };
    }
    if (load_boot_images(images, image_count) != 0)
        goto invalid_args;
    if (initrd && dtb) {
        fdt_patch_u32(dtb_addr, images[dtb_index].size, "linux,initrd-start", initrd_addr);
        fdt_patch_u32(dtb_addr, images[dtb_index].size, "linux,initrd-end",
                      initrd_addr + (uint32_t)images[initrd_index].size);
    }
    free(images);

    clock_gettime(CLOCK_MONOTONIC, &ts);
    boot_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (stats_enabled)
        atexit(print_stats);
    guest_time = 0;
    regs[10] = 0;
    regs[11] = dtb ? dtb_addr : 0;
    priv = 1;
    pc = entry;

    if (!quiet) {
        fprintf(stderr, "tern-linux: kernel 0x%08x dtb 0x%08x entry 0x%08x\n",
                kernel_addr, regs[11], pc);
    }

    for (;;) {
        uint32_t cur_pc, inst, fetch_pa;

        if (max_insn && instret >= max_insn) {
            fprintf(stderr, "\ntern-linux: instruction limit %" PRIu64
                    " reached (pc=0x%08x)\n", max_insn, pc);
            return 2;
        }

        if (maybe_interrupt())
            continue;

        mem_fault = 0;
        mmu_fault_cause = 0;
        if (!translate_addr(pc, ACCESS_FETCH, &fetch_pa) ||
            !phys_in_ram_n(fetch_pa, 4)) {
            if (debug_mmu)
                fprintf(stderr, "mmu: fetch trap pc=0x%08x cause=%u stvec=0x%08x\n",
                        pc, mmu_fault_cause ? mmu_fault_cause : CAUSE_INST_PF, stvec);
            do_trap(mmu_fault_cause ? mmu_fault_cause : CAUSE_INST_PF,
                    pc, 0, pc);
            if (debug_mmu)
                fprintf(stderr, "mmu: redirected pc=0x%08x\n", pc);
            continue;
        }

        cur_pc = pc;
        inst = phys_load32(fetch_pa);
        if (mem_fault) {
            do_trap(CAUSE_INST_PF, pc, 0, pc);
            continue;
        }
        pc = cur_pc + 4;
        instret++;
        cycle++;

        if (trace_count > 0) {
            fprintf(stderr, "pc=0x%08x inst=0x%08x ra=0x%08x sp=0x%08x\n",
                    cur_pc, inst, regs[1], regs[2]);
            trace_count--;
        }
        if (sample_count && (instret % sample_count) == 0) {
            {
                int saved = mem_fault;
                uint32_t caller_ra = 0;
                if (in_ram(regs[8] + 12))
                    caller_ra = mem_load32(regs[8] + 12);
                mem_fault = saved;
                fprintf(stderr, "sample: pc=0x%08x ra=0x%08x caller=0x%08x sp=0x%08x s0=0x%08x a0=0x%08x time=%" PRIu64 " instret=%" PRIu64 "\n",
                        cur_pc, regs[1], caller_ra, regs[2], regs[8], regs[10], guest_time, instret);
            }
        }

        execute(inst, cur_pc);
    }

invalid_args:
    free(images);
    return 2;
}
