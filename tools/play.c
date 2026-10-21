/*
 * RunShot: Live program state capture and playback tool
 *
 * Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Aoran Jiang
 *         Chun Yang
 */

#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>

#include "load.h"
#include "snapshot.h"
#include "xutils.h"

static uint64_t *used_regions_vpn_start = NULL;
static int used_regions_index = 0;
static int region_num;

static bool
is_region_used(uint64_t vpn_start)
{
    ASSERT(region_num > 0);
    for (int i = region_num - 1; i >= 0; i--) {
        if (vpn_start == used_regions_vpn_start[i])
            return true;
        else if (vpn_start > used_regions_vpn_start[i])
            return false;
    }
    return false;
}

static void
load_region(Region *region, bool exec_flag)
{
    void *target_addr = (void *)((region->vpn_start) << PAGE_SHIFT);
    void *mapped_addr = NULL;
    size_t size = (region->vpn_end - region->vpn_start + 1) * PAGE_SIZE;
    bool region_used_flag = 0;
    if (exec_flag) {
        used_regions_vpn_start[used_regions_index++] = region->vpn_start;
    } else {
        region_used_flag = is_region_used(region->vpn_start);
    }

    int prot = PROT_READ | PROT_WRITE;
    if (exec_flag) {
        prot |= PROT_EXEC;
    }

    if (!exec_flag && region_used_flag) {
        mapped_addr = target_addr;
    } else {
        mapped_addr = mmap(target_addr, size, prot,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (mapped_addr == MAP_FAILED) {
            xu_perror("Failed to mmap region at address %p", target_addr);
            xu_exit();
        }
        if (mapped_addr != target_addr) {
            xu_error("Failed to mmap region at address %p (returned %p)",
                     target_addr, mapped_addr);
            xu_exit();
        }
    }

    if (region_used_flag) {
        for (int i = 0; i < region->n_pages; i++) {
            Page *page = region->pages[i];
            uint8_t *handling_addr = (uint8_t *)((page->vpn) << PAGE_SHIFT);
            for (int j = 0; j < PAGE_SIZE; j++) {
                handling_addr[j] |= page->data[j];
            }
        }
    } else {
        for (int i = 0; i < region->n_pages; i++) {
            Page *page = region->pages[i];
            memcpy((void *)((page->vpn) << PAGE_SHIFT), page->data, PAGE_SIZE);
        }
    }
}

__attribute__((noreturn, noinline)) static void
restore_regs_from_start(Registers *regs)
{
    asm volatile("mv sp,   %0\n\t"
                 "mv t0,   %1\n\t"
                 "mv a0, zero\n\t"
                 "mv a1, zero\n\t"
                 "mv a2, zero\n\t"
                 "mv a3, zero\n\t"
                 "mv a4, zero\n\t"
                 "mv a5, zero\n\t"
                 "mv a6, zero\n\t"
                 "mv a7, zero\n\t"
                 "jr t0\n\t"
                 :
                 : "r"(regs->regs[2]), "r"(regs->pc)
                 : "memory");
    __builtin_unreachable();
}

__attribute__((noreturn, noinline)) static void
restore_regs(Registers *regs)
{
    register Registers *base __asm__("ra") = regs;

    asm volatile("fld fs0,  328(%0)\n\t"
                 "fld fs1,  336(%0)\n\t"
                 "fld fs2,  408(%0)\n\t"
                 "fld fs3,  416(%0)\n\t"
                 "fld fs4,  424(%0)\n\t"
                 "fld fs5,  432(%0)\n\t"
                 "fld fs6,  440(%0)\n\t"
                 "fld fs7,  448(%0)\n\t"
                 "fld fs8,  456(%0)\n\t"
                 "fld fs9,  464(%0)\n\t"
                 "fld fs10, 472(%0)\n\t"
                 "fld fs11, 480(%0)\n\t"

                 "ld a0,   520(%0)\n\t"
                 "ld a1,   528(%0)\n\t"
                 "ld a2,   536(%0)\n\t"
                 "csrw fflags,  a0\n\t"
                 "csrw frm,     a1\n\t"
                 "csrw fcsr,    a2\n\t"
                 :
                 : "r"(base)
                 : "memory");

    asm volatile("fld fa0,  344(%0)\n\t"
                 "fld fa1,  352(%0)\n\t"
                 "fld fa2,  360(%0)\n\t"
                 "fld fa3,  368(%0)\n\t"
                 "fld fa4,  376(%0)\n\t"
                 "fld fa5,  384(%0)\n\t"
                 "fld fa6,  392(%0)\n\t"
                 "fld fa7,  400(%0)\n\t"

                 "fld ft0,  264(%0)\n\t"
                 "fld ft1,  272(%0)\n\t"
                 "fld ft2,  280(%0)\n\t"
                 "fld ft3,  288(%0)\n\t"
                 "fld ft4,  296(%0)\n\t"
                 "fld ft5,  304(%0)\n\t"
                 "fld ft6,  312(%0)\n\t"
                 "fld ft7,  320(%0)\n\t"
                 "fld ft8,  488(%0)\n\t"
                 "fld ft9,  496(%0)\n\t"
                 "fld ft10, 504(%0)\n\t"
                 "fld ft11, 512(%0)\n\t"
                 :
                 : "r"(base)
                 : "memory");

    asm volatile("ld s1,   80(%0)\n\t"
                 "ld s2,  152(%0)\n\t"
                 "ld s3,  160(%0)\n\t"
                 "ld s4,  168(%0)\n\t"
                 "ld s5,  176(%0)\n\t"
                 "ld s6,  184(%0)\n\t"
                 "ld s7,  192(%0)\n\t"
                 "ld s8,  200(%0)\n\t"
                 "ld s9,  208(%0)\n\t"
                 "ld s10, 216(%0)\n\t"
                 "ld s11, 224(%0)\n\t"

                 "ld t0,  48(%0)\n\t"
                 "ld t1,  56(%0)\n\t"
                 "ld t2,  64(%0)\n\t"
                 "ld t3, 232(%0)\n\t"
                 "ld t4, 240(%0)\n\t"
                 "ld t5, 248(%0)\n\t"
                 "ld t6, 256(%0)\n\t"
                 :
                 : "r"(base)
                 : "memory");

    asm volatile("ld sp,  24(%0)\n\t"
                 "ld gp,  32(%0)\n\t"
                 "ld tp,  40(%0)\n\t"
                 "ld s0,  72(%0)\n\t"

                 "ld a0,  88(%0)\n\t"
                 "ld a1,  96(%0)\n\t"
                 "ld a2, 104(%0)\n\t"
                 "ld a3, 112(%0)\n\t"
                 "ld a4, 120(%0)\n\t"
                 "ld a5, 128(%0)\n\t"
                 "ld a6, 136(%0)\n\t"
                 "ld a7, 144(%0)\n\t"

                 "ld ra,  16(%0)\n\t"
                 "jr ra\n\t"
                 :
                 : "r"(base)
                 : "memory");

    __builtin_unreachable();
}

void
cmd_play(char *file_name, int verbose)
{
    FILE *fp = NULL;
    fp = fopen(file_name, "rb");
    if (fp == NULL) {
        xu_perror("Failed to open snapshot file '%s'", file_name);
        xu_exit();
    }

    RegionList *insn_regions = NULL;
    RegionList *data_regions = NULL;
    Registers registers;
    read_snapshot_file(fp, &insn_regions, &data_regions, &registers);

    region_num = insn_regions->n_regions;
    used_regions_vpn_start = (uint64_t *)xu_malloc(sizeof(uint64_t)
                                                   * region_num);

    Region *region;
    ASSERT(insn_regions->flags == FLAG_INSN);
    for (int i = 0; i < insn_regions->n_regions; i++) {
        region = insn_regions->regions[i];
        load_region(region, true);
    }
    ASSERT(data_regions->flags == FLAG_DATA);
    for (int i = 0; i < data_regions->n_regions; i++) {
        region = data_regions->regions[i];
        load_region(region, false);
    }

    free_regions(insn_regions);
    insn_regions = NULL;
    free_regions(data_regions);
    data_regions = NULL;

    fclose(fp);
    fp = NULL;

    if (registers.regs[1] == 0) { // ra == 0
        restore_regs_from_start(&registers);
    } else {
        restore_regs(&registers);
    }
}
