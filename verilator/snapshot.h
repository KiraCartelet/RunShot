/*
 * RunShot: Live program state capture and playback tool
 *
 * Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 */

#ifndef __INCLUDE_SNAPSHOT_H__
#define __INCLUDE_SNAPSHOT_H__

#include <stdint.h>

/*
 * Common structures for QEMU plugin and RunShot tools on the RISC-V target.
 * Avoid glib dependency on the RISC-V target.
 */

/*
 * Page - a captured memory page
 */
#define PAGE_SHIFT          12
#define PAGE_SIZE           (1 << PAGE_SHIFT)
#define PAGE_MASK           (~(PAGE_SIZE - 1))
#define PAGE_NUMBER(addr)   ((addr) >> PAGE_SHIFT)
#define PAGE_OFFSET(addr)   ((addr) & (PAGE_SIZE - 1))
#define BLOCK_SHIFT         6
#define BLOCK_SIZE          (1 << BLOCK_SHIFT)
#define N_PAGE_BLOCKS       (PAGE_SIZE / BLOCK_SIZE)
#define BLOCK_INDEX(offset) ((offset) >> BLOCK_SHIFT)
#define BLOCK_POS(offset)   ((offset) & (BLOCK_SIZE - 1))
#define BLOCK_VALID_FLAG    ((uint64_t)(-1))

typedef struct
{
    uint64_t vpn;
    uint64_t bytes_valid[N_PAGE_BLOCKS]; /* bitmap for valid bytes */
    uint8_t data[PAGE_SIZE];
} Page;

/*
 * Registers - captured CPU registers
 */
#define N_RISCV_GPR     32
#define N_RISCV_FP_REGS 32
#define N_RISCV_V_REGS  32
#define N_RISCV_CSR     3

typedef struct
{
    uint64_t pc;
    uint64_t
        regs[N_RISCV_GPR + N_RISCV_FP_REGS]; /* general purpose registers &
                                                fp-extension registers */
    uint64_t csr_regs[N_RISCV_CSR];          /* CSR: fflags, frm, fcsr */
    uint64_t regs_valid;                     /* bitmap for valid registers */
    uint8_t csr_regs_valid;
} Registers;

/*
 * File I/O
 */
#define MAGIC_FILE        0x524e5354 /* "RNST" */
#define MAGIC_PAGE        0x50414745 /* "PAGE" */
#define MAGIC_REGION      0x5245474e /* "REGN" */
#define MAGIC_REGION_LIST 0x52474e4c /* "RGNL" */
#define MAGIC_REGISTERS   0x52454753 /* "REGS" */

typedef struct __attribute__((packed))
{
    uint32_t magic;
    uint32_t size;
} CommonHeader;

typedef struct __attribute__((packed))
{
    CommonHeader common;
    uint64_t vpn;
    uint64_t blocks_valid;
} PageHeader;

typedef struct __attribute__((packed))
{
    CommonHeader common;
    uint64_t vpn_start;
    uint64_t vpn_end;
    uint32_t n_pages;
} RegionHeader;

typedef struct __attribute__((packed))
{
    CommonHeader common;
    uint32_t n_regions;
    uint32_t flags;
} RegionListHeader;

#define REGIONS_FLAG_EXEC_MASK 0x1
#define REGIONS_FLAG_DATA      0x0
#define REGIONS_FLAG_INSN      0x1

typedef struct __attribute__((packed))
{
    CommonHeader common;
    uint64_t pc;
    uint64_t
        regs_valid; /* for 32 general purpose registers & 32 fp registers */
    uint8_t csr_regs_valid;
} RegistersHeader;

#endif /* __INCLUDE_SNAPSHOT_H__ */
