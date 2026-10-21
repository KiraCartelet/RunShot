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
 * Instruction -
 */
#define RV_OPCODE(insn) ((uint32_t)(insn) & 0x7Fu)
#define RV_RD(insn)     (((uint32_t)(insn) >> 7) & 0x1Fu)
#define RV_FUNCT3(insn) (((uint32_t)(insn) >> 12) & 0x7u)
#define RV_RS1(insn)    (((uint32_t)(insn) >> 15) & 0x1Fu)
#define RV_IMM_I(insn)  ((int32_t)((uint32_t)(insn)) >> 20)

#define RV_IS_FUNC_RET(insn)                                                  \
    (RV_OPCODE(insn) == 0x67u && RV_FUNCT3(insn) == 0u && RV_RD(insn) == 0u   \
     && RV_RS1(insn) == 1u && RV_IMM_I(insn) == 0) // ret

typedef struct
{
    uint64_t addr;
    uint32_t insncode;
    char *disas;
} Instruction;

/*
 * File I/O
 */
#define MAGIC_FILE           0x524e5354 /* "RNST" */
#define MAGIC_PAGE           0x50414745 /* "PAGE" */
#define MAGIC_REGION         0x5245474e /* "REGN" */
#define MAGIC_REGION_LIST    0x52474e4c /* "RGNL" */
#define MAGIC_REGISTERS      0x52454753 /* "REGS" */
#define MAGIC_SYSCALL_LOG    0x5359434c /* "SYCL" */
#define MAGIC_SYSCALL_INFO   0x53594349 /* "SYCI" */
#define MAGIC_SYSCALL_BUFFER 0x53594342 /* "SYCB" */

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
#define FLAG_DATA              0x0
#define FLAG_INSN              0x1

typedef struct __attribute__((packed))
{
    CommonHeader common;
    uint64_t pc;
    uint64_t
        regs_valid; /* for 32 general purpose registers & 32 fp registers */
    uint8_t csr_regs_valid;
} RegistersHeader;

typedef struct __attribute__((packed))
{
    CommonHeader common;
    uint32_t n_syscalls;
    uint32_t log_size;
} SyscallLogHeader;

typedef struct __attribute__((packed))
{
    CommonHeader common;
    uint32_t rec_size; /* size of this syscall record, including all buffers */
    int64_t num;       /* syscall number */
    int64_t ret;       /* syscall return value */
    uint16_t n_buffers; /* number of buffers in this syscall */
} SyscallInfoHeader;

typedef struct __attribute__((packed))
{
    CommonHeader common;
    uint64_t addr; /* guest address */
    uint32_t size; /* raw buffer size */
} SyscallBufferHeader;

/*
 * Syscall handler
 */
#define SYSCALL_HANDLER_ADDR   0x80005000ULL
#define SYSCALL_LOG_BASE_ADDR  0x80010000ULL
#define SYSCALL_LOG_FIXED_SIZE 0x100ULL

#endif /* __INCLUDE_SNAPSHOT_H__ */
