/*
 * RunShot: Live program state capture and playback tool
 *
 * Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 */

#ifndef __INCLUDE_SYSCALL_H__
#define __INCLUDE_SYSCALL_H__

#include <glib.h>
#include <stdint.h>

#define __NR_MAX_SYSCALL    512
#define SYSCALL_MAX_ARGS    8
#define SYSCALL_MAX_BUFFERS 4

typedef struct
{
    uint16_t addr_arg;
    uint64_t addr;
    GByteArray buffer;
} SyscallBuffer;

typedef struct
{
    uint64_t pc;
    int64_t num;
    int64_t ret;
    uint16_t n_args;
    uint16_t n_buffers;
    uint64_t args[SYSCALL_MAX_ARGS];
    SyscallBuffer buffers[SYSCALL_MAX_BUFFERS];
} SyscallInfo;

typedef struct
{
    int64_t num;
    uint16_t n_args;
    void (*syscall_ret_cb)(SyscallInfo *info);
} SyscallHandler;

/* Default callback pointer */
#define NO_CALLBACK NULL

/* Installed syscall handlers */
extern SyscallHandler syscall_handlers_installed[];
extern size_t n_syscall_handlers_installed;

#endif /* __INCLUDE_SYSCALL_H__ */
