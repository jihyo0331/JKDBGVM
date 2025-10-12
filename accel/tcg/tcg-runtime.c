/*
 * Tiny Code Generator for QEMU
 *
 * Copyright (c) 2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "exec/cpu-common.h"
#include "exec/helper-proto-common.h"
#include "accel/tcg/getpc.h"
#include "accel/tcg/internal-common.h"
#include "hw/core/cpu.h"
#include "exec/memopidx.h"
#include "exec/tlb-common.h"
#include "exec/tlb-flags.h"
#include "exec/target_page.h"

#define HELPER_H  "accel/tcg/tcg-runtime.h"
#include "exec/helper-info.c.inc"
#undef  HELPER_H

/* 32-bit helpers */

int32_t HELPER(div_i32)(int32_t arg1, int32_t arg2)
{
    return arg1 / arg2;
}

int32_t HELPER(rem_i32)(int32_t arg1, int32_t arg2)
{
    return arg1 % arg2;
}

uint32_t HELPER(divu_i32)(uint32_t arg1, uint32_t arg2)
{
    return arg1 / arg2;
}

uint32_t HELPER(remu_i32)(uint32_t arg1, uint32_t arg2)
{
    return arg1 % arg2;
}

/* 64-bit helpers */

uint64_t HELPER(shl_i64)(uint64_t arg1, uint64_t arg2)
{
    return arg1 << arg2;
}

uint64_t HELPER(shr_i64)(uint64_t arg1, uint64_t arg2)
{
    return arg1 >> arg2;
}

int64_t HELPER(sar_i64)(int64_t arg1, int64_t arg2)
{
    return arg1 >> arg2;
}

int64_t HELPER(div_i64)(int64_t arg1, int64_t arg2)
{
    return arg1 / arg2;
}

int64_t HELPER(rem_i64)(int64_t arg1, int64_t arg2)
{
    return arg1 % arg2;
}

uint64_t HELPER(divu_i64)(uint64_t arg1, uint64_t arg2)
{
    return arg1 / arg2;
}

uint64_t HELPER(remu_i64)(uint64_t arg1, uint64_t arg2)
{
    return arg1 % arg2;
}

uint64_t HELPER(muluh_i64)(uint64_t arg1, uint64_t arg2)
{
    uint64_t l, h;
    mulu64(&l, &h, arg1, arg2);
    return h;
}

int64_t HELPER(mulsh_i64)(int64_t arg1, int64_t arg2)
{
    uint64_t l, h;
    muls64(&l, &h, arg1, arg2);
    return h;
}

uint32_t HELPER(clz_i32)(uint32_t arg, uint32_t zero_val)
{
    return arg ? clz32(arg) : zero_val;
}

uint32_t HELPER(ctz_i32)(uint32_t arg, uint32_t zero_val)
{
    return arg ? ctz32(arg) : zero_val;
}

uint64_t HELPER(clz_i64)(uint64_t arg, uint64_t zero_val)
{
    return arg ? clz64(arg) : zero_val;
}

uint64_t HELPER(ctz_i64)(uint64_t arg, uint64_t zero_val)
{
    return arg ? ctz64(arg) : zero_val;
}

uint32_t HELPER(clrsb_i32)(uint32_t arg)
{
    return clrsb32(arg);
}

uint64_t HELPER(clrsb_i64)(uint64_t arg)
{
    return clrsb64(arg);
}

uint32_t HELPER(ctpop_i32)(uint32_t arg)
{
    return ctpop32(arg);
}

uint64_t HELPER(ctpop_i64)(uint64_t arg)
{
    return ctpop64(arg);
}

void HELPER(exit_atomic)(CPUArchState *env)
{
    cpu_loop_exit_atomic(env_cpu(env), GETPC());
}

void HELPER(log_store_fastpath)(CPUArchState *env, uint64_t addr,
                                uint64_t value_lo, uint64_t value_hi,
                                uint32_t oi)
{
#ifdef CONFIG_USER_ONLY
    (void)env;
    (void)addr;
    (void)value_lo;
    (void)value_hi;
    (void)oi;
    return;
#else
    if (!mmu_fast_log_enabled) {
        return;
    }

    CPUState *cpu = env_cpu(env);
    MemOp memop = get_memop(oi);
    unsigned size_shift = memop & MO_SIZE;
    unsigned size = (size_shift == MO_128) ? 16u : 1u << size_shift;
    unsigned mmu_idx = get_mmuidx(oi);
    CPUTLBDescFast *fast = &cpu->neg.tlb.f[mmu_idx];
    uintptr_t index_mask = fast->mask >> CPU_TLB_ENTRY_BITS;

    if (fast->table == NULL || index_mask == 0) {
        return;
    }

    uintptr_t index = (addr >> TARGET_PAGE_BITS) & index_mask;
    CPUTLBEntry *entry = &fast->table[index];
    uint64_t tlb_addr = entry->addr_write;

    if (((tlb_addr ^ addr) & TARGET_PAGE_MASK) ||
        (tlb_addr & TLB_INVALID_MASK)) {
        return;
    }

    CPUTLBEntryFull *full = &cpu->neg.tlb.d[mmu_idx].fulltlb[index];
    hwaddr phys = full->phys_addr + (addr & ~TARGET_PAGE_MASK);

    switch (size) {
    case 1:
        printf("[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
               " paddr=0x%016" PRIx64 " size=1 data=0x%02" PRIx64 "\n",
               addr, phys, value_lo & UINT8_MAX);
        if (mmu_log_to_file && mmu_log_file) {
            fprintf(mmu_log_file,
                    "[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
                    " paddr=0x%016" PRIx64 " size=1 data=0x%02" PRIx64 "\n",
                    addr, phys, value_lo & UINT8_MAX);
            fflush(mmu_log_file);
        }
        break;
    case 2:
        printf("[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
               " paddr=0x%016" PRIx64 " size=2 data=0x%04" PRIx64 "\n",
               addr, phys, value_lo & UINT16_MAX);
        if (mmu_log_to_file && mmu_log_file) {
            fprintf(mmu_log_file,
                    "[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
                    " paddr=0x%016" PRIx64 " size=2 data=0x%04" PRIx64 "\n",
                    addr, phys, value_lo & UINT16_MAX);
            fflush(mmu_log_file);
        }
        break;
    case 4:
        printf("[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
               " paddr=0x%016" PRIx64 " size=4 data=0x%08" PRIx64 "\n",
               addr, phys, value_lo & UINT32_MAX);
        if (mmu_log_to_file && mmu_log_file) {
            fprintf(mmu_log_file,
                    "[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
                    " paddr=0x%016" PRIx64 " size=4 data=0x%08" PRIx64 "\n",
                    addr, phys, value_lo & UINT32_MAX);
            fflush(mmu_log_file);
        }
        break;
    case 8:
        printf("[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
               " paddr=0x%016" PRIx64 " size=8 data=0x%016" PRIx64 "\n",
               addr, phys, value_lo);
        if (mmu_log_to_file && mmu_log_file) {
            fprintf(mmu_log_file,
                    "[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
                    " paddr=0x%016" PRIx64 " size=8 data=0x%016" PRIx64 "\n",
                    addr, phys, value_lo);
            fflush(mmu_log_file);
        }
        break;
    case 16:
        printf("[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
               " paddr=0x%016" PRIx64 " size=16 data=0x%016" PRIx64
               "%016" PRIx64 "\n",
               addr, phys, value_hi, value_lo);
        if (mmu_log_to_file && mmu_log_file) {
            fprintf(mmu_log_file,
                    "[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
                    " paddr=0x%016" PRIx64 " size=16 data=0x%016" PRIx64
                    "%016" PRIx64 "\n",
                    addr, phys, value_hi, value_lo);
            fflush(mmu_log_file);
        }
        break;
    default:
        printf("[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
               " paddr=0x%016" PRIx64 " size=%u data=0x%016" PRIx64 "\n",
               addr, phys, size, value_lo);
        if (mmu_log_to_file && mmu_log_file) {
            fprintf(mmu_log_file,
                    "[FAST_MEMORY_WRITE] vaddr=0x%016" PRIx64
                    " paddr=0x%016" PRIx64 " size=%u data=0x%016" PRIx64 "\n",
                    addr, phys, size, value_lo);
            fflush(mmu_log_file);
        }
        break;
    }
#endif
}
