/*
 * RunShot: Live program state capture and playback tool
 *
 * Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 */

#ifndef __QEMU_PLUGINS_IMAGE_H__
#define __QEMU_PLUGINS_IMAGE_H__

#include <glib.h>
#include <stdint.h>
#include <stdio.h>

#include <qemu-plugin.h>

#include "snapshot.h"
#include "syscall.h"

/*
 * Region - a region of continuous pages
 */
/* Maximum gap pages allowed during region merging */
#define REGION_MAX_GAP 4

typedef struct
{
    uint64_t vpn_start;
    uint64_t vpn_end;
    GPtrArray *pages;
} Region;

/* Region interfaces */
GArray *merge_pages_to_regions(GHashTable *hash_pages);
void write_regions(FILE *fp, GArray *regions, uint32_t flags);
void free_regions(GArray *regions);

/* Register interfaces */
void write_registers(FILE *fp, Registers *registers);

void print_debug(const char *fmt, ...);

/*
 * Syscall log & handler
 */
/* Syscall log mini-header addr offset from SYSCALL_LOG_BASE_ADDR */
#define LOG_CURSOR   0x00
#define LOG_START    0x08
#define LOG_END      0x10
#define RETADDR_SLOT 0x18

/* Scratch registers: The offset when saving/restoring regs in syscall handling
 */
#define SAVE_T0 0x20
#define SAVE_T1 0x28
#define SAVE_T2 0x30
#define SAVE_T3 0x38
#define SAVE_T4 0x40
#define SAVE_T5 0x48
#define SAVE_T6 0x50
#define SAVE_A1 0x58
#define SAVE_A2 0x60
#define SAVE_A3 0x68
#define SAVE_A4 0x70
#define SAVE_A5 0x78
#define SAVE_A6 0x80

enum
{
    X_ZERO = 0,
    X_RA = 1,
    X_SP = 2,
    X_GP = 3,
    X_TP = 4,
    X_T0 = 5,
    X_T1 = 6,
    X_T2 = 7,
    X_S0 = 8,
    X_S1 = 9,
    X_A0 = 10,
    X_A1 = 11,
    X_A2 = 12,
    X_A3 = 13,
    X_A4 = 14,
    X_A5 = 15,
    X_A6 = 16,
    X_A7 = 17,
    X_S2 = 18,
    X_S3 = 19,
    X_S4 = 20,
    X_S5 = 21,
    X_S6 = 22,
    X_S7 = 23,
    X_S8 = 24,
    X_S9 = 25,
    X_S10 = 26,
    X_S11 = 27,
    X_T3 = 28,
    X_T4 = 29,
    X_T5 = 30,
    X_T6 = 31,
};

typedef struct
{
    Page *page;
    uint64_t base;
    uint64_t pc;
} InsnEmitter;

/* Syscall log & handler interfaces */
void inject_syscall_handler_page(GArray *insn_regions, uint64_t handler_addr);

void patch_syscall_handler(GArray *insn_regions, GPtrArray *syscalls,
                           uint32_t pass_syscall,
                           uint64_t replay_handler_addr);

void inject_syscall_log(GArray *data_regions, GPtrArray *syscalls,
                        uint32_t pass_syscall, uint64_t base_addr);

#endif /* __QEMU_PLUGINS_IMAGE_H__ */
