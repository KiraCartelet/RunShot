/*
 * RunShot: Live program state capture and playback tool
 *
 * Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 */

#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdio.h>

/* QEMU */
#include <linux-headers/asm-riscv/unistd_64.h>
#include <qemu-plugin.h>

#include "snapshot.h"
#include "syscall.h"

/* Save data in callback function */
extern GMutex data_pages_lock;
extern GHashTable *data_pages;
Page *get_page(GHashTable *pages, uint64_t vpn);
void capture_load_in_page(Page *page, uint64_t vaddr, uint8_t *data,
                          size_t size, uint32_t flag);

bool verbose = 1;

#define DUMP_MAX 256
static void
print_data_with_max_dump(int size, guint8 *data)
{
    if (!verbose)
        return;
    /* Print first DUMP_MAX Bytes */
    uint64_t dump_num = size < DUMP_MAX ? size : DUMP_MAX;
    printf("  data (first %llu Bytes):", (unsigned long long)dump_num);
    for (uint64_t i = 0; i < dump_num; i++) {
        printf(" 0x%02x", data[i]);
    }
    if (dump_num < size) {
        printf(" ...");
    }
    printf("\n");
}

static void
openat_cb(SyscallInfo *info)
{
    uint64_t dir_fd = info->args[0];
    uint64_t pathname_addr = info->args[1];
    uint64_t flags = info->args[2];
    uint64_t mode = info->args[3];
    uint64_t ret = info->ret;
    SyscallBuffer *buf = &info->buffers[0];
    uint64_t size = strnlen((const char *)pathname_addr,
                            1024 - 1); // max size of pathname

    info->n_buffers = 1;
    buf->addr_arg = 1;
    buf->addr = pathname_addr;
    buf->buffer.len = size;
    buf->buffer.data = g_malloc(size);
    assert(buf->buffer.data != NULL);
    qemu_plugin_read_memory_vaddr(pathname_addr, &buf->buffer, size);

    /* Save pathname */
    uint64_t vpn = PAGE_NUMBER(pathname_addr);
    uint8_t *addr_buf = (uint8_t *)g_malloc((size + 1) * sizeof(uint8_t));
    memcpy(addr_buf, (const void *)pathname_addr, size);
    addr_buf[size] = '\0';

    g_mutex_lock(&data_pages_lock);
    Page *page = get_page(data_pages, vpn);
    capture_load_in_page(page, pathname_addr, addr_buf, size + 1, FLAG_INSN);
    g_mutex_unlock(&data_pages_lock);

    g_free(addr_buf);

    printf("Syscall: openat (__NR=%d)\n", __NR_openat);
    printf(" dir_fd : %ld\n", (int64_t)dir_fd);
    printf(" path : 0x%lx\n", pathname_addr);
    printf(" flags : 0x%lx\n", flags);
    printf(" mode : 0x%lx\n", mode);
    printf(" ret : %ld\n", (int64_t)ret);
}

static void
read_cb(SyscallInfo *info)
{
    int64_t ret = info->ret; /* Actual number of bytes read */
    uint64_t fd = info->args[0];
    uint64_t addr = info->args[1];
    uint64_t req = info->args[2]; /* User required size */

    if (verbose) {
        printf("Syscall: read (__NR=%d)\n", __NR_read);
        printf("  fd: 0x%lx\n", fd);
        printf("  addr: 0x%lx\n", addr);
        printf("  req: %llu\n", (unsigned long long)req);
        printf("  ret: %ld\n", ret);
    }
    if (ret <= 0)
        return;

    uint64_t size = (uint64_t)ret;
    SyscallBuffer *buf = &info->buffers[0];

    info->n_buffers = 1;
    buf->addr_arg = 1;
    buf->addr = addr;
    buf->buffer.len = size;
    buf->buffer.data = g_malloc(size);
    assert(buf->buffer.data != NULL);

    qemu_plugin_read_memory_vaddr(addr, &buf->buffer, size);

    print_data_with_max_dump(size, buf->buffer.data);
}

static void
write_cb(SyscallInfo *info)
{
    int64_t ret = info->ret;
    uint64_t fd = info->args[0];
    uint64_t addr = info->args[1];
    uint64_t req = info->args[2];

    if (verbose) {
        printf("Syscall: write (__NR=%d)\n", __NR_write);
        printf("  fd: 0x%lx\n", fd);
        printf("  addr: 0x%lx\n", addr);
        printf("  req: %llu\n", (unsigned long long)req);
        printf("  ret: %ld\n", ret);
    }
    if (ret <= 0)
        return;

    uint64_t size = (uint64_t)ret;
    SyscallBuffer *buf = &info->buffers[0];

    info->n_buffers = 1;
    buf->addr_arg = 1;
    buf->addr = addr;
    buf->buffer.len = size;
    buf->buffer.data = g_malloc(size);
    assert(buf->buffer.data);

    qemu_plugin_read_memory_vaddr(addr, &buf->buffer, size);

    print_data_with_max_dump(size, buf->buffer.data);
}

static void
readlinkat_cb(SyscallInfo *info)
{
    uint64_t dir_fd = info->args[0];
    uint64_t pathname_addr = info->args[1];
    uint64_t addr = info->args[2];
    uint64_t bufsize = info->args[3];
    uint64_t ret = info->ret;
    SyscallBuffer *buf = &info->buffers[0];
    uint64_t size = strnlen((const char *)pathname_addr, bufsize - 1);

    info->n_buffers = 1;
    buf->addr_arg = 2;
    buf->addr = addr;
    buf->buffer.len = size;
    buf->buffer.data = g_malloc(size);
    assert(buf->buffer.data != NULL);

    qemu_plugin_read_memory_vaddr(addr, &buf->buffer, size);

    /* Save pathname */
    uint64_t vpn = PAGE_NUMBER(pathname_addr);
    uint8_t *addr_buf = (uint8_t *)g_malloc((size + 1) * sizeof(uint8_t));
    memcpy(addr_buf, (const void *)pathname_addr, size);
    addr_buf[size] = '\0';

    g_mutex_lock(&data_pages_lock);
    Page *page = get_page(data_pages, vpn);
    capture_load_in_page(page, pathname_addr, addr_buf, size + 1, FLAG_INSN);
    g_mutex_unlock(&data_pages_lock);

    g_free(addr_buf);

    printf("Syscall: readlinkat (__NR=%d)\n", __NR_readlinkat);
    printf(" dir_fd : %ld\n", (int64_t)dir_fd);
    printf(" path: 0x%lx\n", pathname_addr);
    printf(" buf: 0x%lx\n", addr);
    printf(" bufsize: 0x%lx\n", bufsize);
    printf(" ret: %ld\n", (int64_t)ret);
}

static void
newfstatat_cb(SyscallInfo *info)
{
    int64_t ret = info->ret;
    uint64_t dirfd = info->args[0];
    uint64_t pathname_addr = info->args[1];
    uint64_t statbuf_addr = info->args[2];
    uint64_t flags = info->args[3];

    if (verbose) {
        printf("Syscall: newfstatat (__NR=%d)\n", __NR_newfstatat);
        printf("  dirfd: 0x%lx\n", dirfd);
        printf("  pathname: 0x%lx\n", pathname_addr);
        printf("  statbuf: 0x%lx\n", statbuf_addr);
        printf("  flags: 0x%lx\n", flags);
        printf("  ret: %ld\n", ret);
    }

    if (ret != 0)
        return;

    const uint64_t size = 144; /* sizeof(struct stat) */

    SyscallBuffer *buf = &info->buffers[0];
    info->n_buffers = 1;
    buf->addr_arg = 2;
    buf->addr = statbuf_addr;
    buf->buffer.len = size;
    buf->buffer.data = g_malloc(size);
    assert(buf->buffer.data != NULL);

    qemu_plugin_read_memory_vaddr(statbuf_addr, &buf->buffer, size);

    print_data_with_max_dump(size, buf->buffer.data);
}

static void
fstat_cb(SyscallInfo *info)
{
    int64_t ret = info->ret;
    uint64_t fd = info->args[0];
    uint64_t addr = info->args[1];

    if (verbose) {
        printf("Syscall: fstat (__NR=%d)\n", __NR_fstat);
        printf("  fd: 0x%lx\n", fd);
        printf("  buf: 0x%lx\n", addr);
        printf("  ret: %ld\n", ret);
    }

    if (ret != 0)
        return;

    const uint64_t size = 144; /* sizeof(struct stat) */

    SyscallBuffer *buf = &info->buffers[0];
    info->n_buffers = 1;
    buf->addr_arg = 1;
    buf->addr = addr;
    buf->buffer.len = size;
    buf->buffer.data = g_malloc(size);
    assert(buf->buffer.data != NULL);

    qemu_plugin_read_memory_vaddr(addr, &buf->buffer, size);

    print_data_with_max_dump(size, buf->buffer.data);
}

static void
clock_gettime_cb(SyscallInfo *info)
{
    int64_t ret = info->ret;
    uint64_t clockid = info->args[0];
    uint64_t addr = info->args[1];

    if (verbose) {
        printf("Syscall: clock_gettime (__NR=%d)\n", __NR_clock_gettime);
        printf("  clockid: %llu\n", (unsigned long long)clockid);
        printf("  addr: 0x%lx\n", addr);
        printf("  ret: %ld\n", ret);
    }

    if (ret != 0)
        return;

    uint64_t size = 16; /* sizeof(TargetTimespec) */
    SyscallBuffer *buf = &info->buffers[0];

    info->n_buffers = 1;
    buf->addr_arg = 1;
    buf->addr = addr;
    buf->buffer.len = size;
    buf->buffer.data = g_malloc(size);
    assert(buf->buffer.data);

    qemu_plugin_read_memory_vaddr(addr, &buf->buffer, size);

    print_data_with_max_dump(size, buf->buffer.data);
}

static void
brk_cb(SyscallInfo *info)
{
    uint64_t ret = info->ret;
    uint64_t addr = info->args[0];

    info->n_buffers = 0;

    if (verbose) {
        printf("Syscall: brk (__NR=%d)\n", __NR_brk);
        printf("  addr: 0x%lx\n", addr);
        printf("  ret: 0x%lx\n", ret);
    }
}

static void
riscv_hwprobe_cb(SyscallInfo *info)
{
    uint64_t pair_size = 16; /* sizeof(struct riscv_hwprobe) */
    uint64_t addr = info->args[0];
    uint64_t size = pair_size * info->args[1];
    SyscallBuffer *buf = &info->buffers[0];

    info->n_buffers = 1;
    buf->addr_arg = 0;
    buf->addr = addr;
    buf->buffer.len = size;
    buf->buffer.data = g_malloc(size);
    assert(buf->buffer.data != NULL);
    qemu_plugin_read_memory_vaddr(addr, &buf->buffer, size);

    printf("Syscall: riscv_hwprobe\n");
    printf("  addr: 0x%lx\n", addr);
    printf("  size: 0x%lx\n", size);
    printf("  data: ");
    for (int i = 0; i < size; i++) {
        printf(" 0x%02x", buf->buffer.data[i]);
    }
    printf("\n");
}

static void
prlimit64_cb(SyscallInfo *info)
{
    int64_t ret = info->ret;
    uint64_t pid = info->args[0];
    uint64_t resource = info->args[1];
    uint64_t new_p = info->args[2];
    uint64_t old_p = info->args[3];

    if (verbose) {
        printf("Syscall: prlimit64 (__NR=%d)\n", __NR_prlimit64);
        printf("  pid: 0x%lx\n", pid);
        printf("  resource: 0x%lx\n", resource);
        printf("  new_rlim: 0x%lx\n", new_p);
        printf("  old_rlim: 0x%lx\n", old_p);
        printf("  ret: %ld\n", ret);
    }

    if (ret < 0 || !old_p)
        return;

    const uint64_t size = 16; /* sizeof(struct rlimit64) */

    SyscallBuffer *buf = &info->buffers[0];
    info->n_buffers = 1;
    buf->addr_arg = 3;
    buf->addr = old_p;
    buf->buffer.len = size;
    buf->buffer.data = g_malloc(size);
    assert(buf->buffer.data != NULL);

    qemu_plugin_read_memory_vaddr(old_p, &buf->buffer, size);

    print_data_with_max_dump(size, buf->buffer.data);
}

static void
getrandom_cb(SyscallInfo *info)
{
    long ret = info->ret;
    uint64_t addr = info->args[0];
    uint64_t buflen = info->args[1];
    uint64_t flags = info->args[2];

    if (verbose) {
        printf("Syscall: getrandom (__NR=%d)\n", __NR_getrandom);
        printf("  addr: 0x%lx\n", addr);
        printf("  buf length: 0x%lx\n", buflen);
        printf("  flags: 0x%lx\n", flags);
        printf("  ret: %ld\n", ret);
    }

    if (ret <= 0)
        return;

    uint64_t size = (uint64_t)ret;
    SyscallBuffer *buf = &info->buffers[0];

    info->n_buffers = 1;
    buf->addr_arg = 0;
    buf->addr = addr;
    buf->buffer.len = size;
    buf->buffer.data = g_malloc(size);
    assert(buf->buffer.data != NULL);

    qemu_plugin_read_memory_vaddr(addr, &buf->buffer, size);

    print_data_with_max_dump(size, buf->buffer.data);
}

SyscallHandler syscall_handlers_installed[] = {
    {__NR_openat, 4, openat_cb},               // 56
    {__NR_close, 1, NO_CALLBACK},              // 57
    {__NR_lseek, 3, NO_CALLBACK},              // 62
    {__NR_read, 3, read_cb},                   // 63
    {__NR_write, 3, write_cb},                 // 64
    {__NR_readlinkat, 4, readlinkat_cb},       // 78
    {__NR_newfstatat, 4, newfstatat_cb},       // 79
    {__NR_fstat, 2, fstat_cb},                 // 80
    {__NR_exit, 1, NO_CALLBACK},               // 93
    {__NR_exit_group, 1, NO_CALLBACK},         // 94
    {__NR_set_tid_address, 1, NO_CALLBACK},    // 96
    {__NR_set_robust_list, 0, NO_CALLBACK},    // 99
    {__NR_clock_gettime, 2, clock_gettime_cb}, // 113
    {__NR_brk, 1, brk_cb},                     // 214
    {__NR_munmap, 2, NO_CALLBACK},             // 215
    {__NR_mmap, 6, NO_CALLBACK},               // 222
    {__NR_mprotect, 3, NO_CALLBACK},           // 226
    {__NR_riscv_hwprobe, 5, riscv_hwprobe_cb}, // 258
    {__NR_prlimit64, 4, prlimit64_cb},         // 261
    {__NR_getrandom, 3, getrandom_cb},         // 278
};

size_t n_syscall_handlers_installed = sizeof(syscall_handlers_installed)
                                      / sizeof(SyscallHandler);
