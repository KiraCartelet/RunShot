/*
 * RunShot: Live program state capture and playback tool
 *
 * Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 *         Aoran Jiang
 */

#include <assert.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>

/* QEMU */
#include <qemu-plugin.h>
#include <sys/types.h>

#include "image.h"
#include "snapshot.h"
#include "syscall.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* for short code */
#define _GS_PRINTF g_string_append_printf

/* Instruction and data pages */
static GMutex insn_pages_lock;
static GHashTable *insn_pages;
GMutex data_pages_lock;
GHashTable *data_pages;

/* Registers */
static GArray *regs_list;
static Registers *registers;

/* Syscalls */
static GPtrArray *syscalls;
static SyscallHandler syscall_handlers[__NR_MAX_SYSCALL];
static uint32_t pass_syscall = 0;
static uint64_t last_pc = 0;

/* Breakpoint */
static uint64_t breakpoint = 1;
static uint64_t counter = 0;

/* Elf parse */
static bool plugin_start_flag = TRUE;
static char *elf_path = NULL;
static uint64_t plugin_start_addr = 0;
uint64_t elf_find_start_addr(const char *path);

/* GUI */
static bool mode = 1; // mode 0: print all insns; mode 1: save breakpoint
static FILE *fp_assembly = NULL;
static uint64_t insnnum = 0;

/*
 * RISC-V register aliases
 */
typedef struct
{
    const char *name;
    int index;
} RegisterAlias;

void
print_debug(const char *fmt, ...)
{
    GString *out = g_string_new("");
    va_list args;
    va_start(args, fmt);
    g_string_append_vprintf(out, fmt, args);
    va_end(args);

    qemu_plugin_outs(out->str);
    g_string_free(out, TRUE);
}

static const RegisterAlias riscv_reg_map[] = {
    {"zero", 0},   {"ra", 1},    {"sp", 2},   {"gp", 3},   {"tp", 4},

    {"t0", 5},     {"t1", 6},    {"t2", 7},   {"t3", 28},  {"t4", 29},
    {"t5", 30},    {"t6", 31},

    {"s0", 8},     {"fp", 8},    {"s1", 9},   {"s2", 18},  {"s3", 19},
    {"s4", 20},    {"s5", 21},   {"s6", 22},  {"s7", 23},  {"s8", 24},
    {"s9", 25},    {"s10", 26},  {"s11", 27},

    {"a0", 10},    {"a1", 11},   {"a2", 12},  {"a3", 13},  {"a4", 14},
    {"a5", 15},    {"a6", 16},   {"a7", 17},

    {"ft0", 32},   {"ft1", 33},  {"ft2", 34}, {"ft3", 35}, {"ft4", 36},
    {"ft5", 37},   {"ft6", 38},  {"ft7", 39}, {"ft8", 60}, {"ft9", 61},
    {"ft10", 62},  {"ft11", 63},

    {"fs0", 40},   {"fs1", 41},  {"fs2", 50}, {"fs3", 51}, {"fs4", 52},
    {"fs5", 53},   {"fs6", 54},  {"fs7", 55}, {"fs8", 56}, {"fs9", 57},
    {"fs10", 58},  {"fs11", 59},

    {"fa0", 42},   {"fa1", 43},  {"fa2", 44}, {"fa3", 45}, {"fa4", 46},
    {"fa5", 47},   {"fa6", 48},  {"fa7", 49},

    {"fflags", 0}, {"frm", 1},   {"fcsr", 2},
};

static int
riscv_reg_index(const char *name)
{
    for (size_t i = 0; i < G_N_ELEMENTS(riscv_reg_map); i++) {
        if (g_strcmp0(name, riscv_reg_map[i].name) == 0) {
            return riscv_reg_map[i].index;
        }
    }

    return -1;
}

static void
get_registers(Registers *registers, GArray *regs_list)
{
    uint64_t value;
    GByteArray *value_buf = g_byte_array_new();

    assert(registers != NULL && regs_list != NULL);
    for (int i = 0; i < N_RISCV_GPR + N_RISCV_FP_REGS + 1; i++) {
        qemu_plugin_reg_descriptor *desc = &g_array_index(
            regs_list, qemu_plugin_reg_descriptor, i);

        int size = qemu_plugin_read_register(desc->handle, value_buf);
        assert(size > 0 && size <= 8);
        memcpy(&value, value_buf->data, size);

        if (g_strcmp0(desc->name, "pc") == 0) {
            registers->pc = value;
        } else {
            int index = riscv_reg_index(desc->name);
            assert(index >= 0);
            registers->regs[index] = value;
            registers->regs_valid |= (1ULL << index);
            // g_autoptr(GString) out = g_string_new("");

            //_GS_PRINTF(out, "name:%s, value:%"PRIx64"\n", desc->name,value);
            // qemu_plugin_outs(out->str);
        }

        g_byte_array_set_size(value_buf, 0);
    }

    /* CSR: fflags, frm, fcsr */
    for (int i = N_RISCV_GPR + N_RISCV_FP_REGS + N_RISCV_V_REGS + 2;
         i < N_RISCV_GPR + N_RISCV_FP_REGS + N_RISCV_V_REGS + N_RISCV_CSR + 2;
         i++) {
        qemu_plugin_reg_descriptor *desc = &g_array_index(
            regs_list, qemu_plugin_reg_descriptor, i);

        int size = qemu_plugin_read_register(desc->handle, value_buf);
        assert(size > 0 && size <= 8);
        memcpy(&value, value_buf->data, size);

        int index = riscv_reg_index(desc->name);
        assert(index >= 0);
        registers->csr_regs[index] = value;
        registers->csr_regs_valid |= (1ULL << index);
        // g_autoptr(GString) out = g_string_new("");

        //_GS_PRINTF(out, "name:%s, value:%"PRIx64"\n", desc->name,value);
        // qemu_plugin_outs(out->str);
        g_byte_array_set_size(value_buf, 0);
    }

    g_byte_array_unref(value_buf);
    value_buf = NULL;
}

static void
vcpu_init(qemu_plugin_id_t id, unsigned int cpu_index)
{
    regs_list = qemu_plugin_get_registers();
}

Page *
get_page(GHashTable *pages, uint64_t vpn)
{
    Page *page = (Page *)g_hash_table_lookup(pages, GUINT_TO_POINTER(vpn));
    if (page == NULL) {
        page = g_new0(Page, 1);
        page->vpn = vpn;
        g_hash_table_insert(pages, GUINT_TO_POINTER(page->vpn), page);
    }

    return page;
}

void
capture_load_in_page(Page *page, uint64_t vaddr, uint8_t *data, size_t size,
                     uint32_t flag)
{
    uint64_t offset = PAGE_OFFSET(vaddr);

    for (size_t i = 0; i < size; i++) {
        int index = BLOCK_INDEX(offset + i);
        int pos = BLOCK_POS(offset + i);
        if ((flag == FLAG_DATA && counter < breakpoint)
            || (page->bytes_valid[index] & (1ULL << pos)))
            continue;

        /* The first load */
        page->data[offset + i] = data[i];
        page->bytes_valid[index] |= 1ULL << pos;
    }
}

void
capture_store_in_page(Page *page, uint64_t vaddr, size_t size)
{
    uint64_t offset = PAGE_OFFSET(vaddr);

    for (size_t i = 0; i < size; i++) {
        int index = BLOCK_INDEX(offset + i);
        int pos = BLOCK_POS(offset + i);

        if (counter < breakpoint)
            continue;
        page->bytes_valid[index] |= 1ULL << pos;
    }
}

static void
vcpu_insn_exec_assembly_print(unsigned int cpu_index, void *udata)
{
    Instruction *pinsn = (Instruction *)udata;
    last_pc = pinsn->addr;
    if (pinsn->addr == plugin_start_addr) {
        // print_debug("addr:%p\n",plugin_start_addr);
        plugin_start_flag = TRUE;
    }
    if (plugin_start_flag == FALSE)
        return;
    fprintf(fp_assembly, "0x%08lx:  %08x  %s\n", pinsn->addr, pinsn->insncode,
            pinsn->disas);
    if (RV_IS_FUNC_RET(pinsn->insncode))
        counter++;
    if (counter >= breakpoint)
        insnnum++;
}

static void
vcpu_insn_exec(unsigned int cpu_index, void *udata)
{
    Instruction *pinsn = (Instruction *)udata;
    last_pc = pinsn->addr;
    if (pinsn->addr == plugin_start_addr) {
        // print_debug("addr:%p\n",plugin_start_addr);
        plugin_start_flag = TRUE;
    }
    if (plugin_start_flag == FALSE)
        return;
    if (registers->regs_valid == 0 && breakpoint == 0) {
        get_registers(registers, regs_list);
        registers->pc = pinsn->addr;
    }
    if (RV_IS_FUNC_RET(pinsn->insncode)) {
        if (++counter == breakpoint) {
            get_registers(registers, regs_list);
            registers->pc = pinsn->addr;
        }
    }
    if (counter >= breakpoint)
        insnnum++;
}

static void
vcpu_mem(unsigned int cpu_index, qemu_plugin_meminfo_t info, uint64_t vaddr,
         void *udata)
{
    bool is_store = qemu_plugin_mem_is_store(info);
    qemu_plugin_mem_value value = qemu_plugin_mem_get_value(info);
    size_t data_size = 1 << qemu_plugin_mem_size_shift(info);
    uint64_t vpn_start = PAGE_NUMBER(vaddr);
    uint64_t vpn_end = PAGE_NUMBER(vaddr + data_size - 1);

    assert(vpn_end - vpn_start <= 1);
    size_t offset = PAGE_OFFSET(vaddr);
    uint8_t *data = (uint8_t *)&value.data;
    g_mutex_lock(&data_pages_lock);
    for (uint64_t vpn = vpn_start; vpn <= vpn_end; vpn++) {
        size_t size = vpn == vpn_end ? data_size : PAGE_SIZE - offset;

        Page *page = get_page(data_pages, vpn);
        if (is_store) {
            capture_store_in_page(page, vaddr, size);
        } else {
            capture_load_in_page(page, vaddr, data, size, FLAG_DATA);
        }

        vaddr += size;
        data += size;
        data_size -= size;
        offset = 0;
    }
    g_mutex_unlock(&data_pages_lock);
}

/* On translation of a new TB */
static void
vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint8_t insn_word[4];
        size_t insn_size = qemu_plugin_insn_size(insn);
        uint64_t vaddr = qemu_plugin_insn_vaddr(insn);
        uint64_t vpn_start = PAGE_NUMBER(vaddr);
        uint64_t vpn_end = PAGE_NUMBER(vaddr + insn_size - 1);

        /* Capture the instruction. */
        assert(insn_size <= 4 && vpn_end - vpn_start <= 1);
        qemu_plugin_insn_data(insn, insn_word, insn_size);

        /* Fix PC bug in qemu_plugin_read_register() */
        Instruction *pinsn = g_new(Instruction, 1);
        pinsn->addr = vaddr;
        pinsn->insncode = *((uint32_t *)insn_word);

        size_t offset = PAGE_OFFSET(vaddr);
        uint8_t *insn_data = insn_word;
        g_mutex_lock(&insn_pages_lock);
        for (uint64_t vpn = vpn_start; vpn <= vpn_end; vpn++) {
            size_t size = vpn == vpn_end ? insn_size : PAGE_SIZE - offset;

            Page *page = get_page(insn_pages, vpn);
            capture_load_in_page(page, vaddr, insn_data, size, FLAG_INSN);

            vaddr += size;
            insn_data += size;
            insn_size -= size;
            offset = 0;
        }
        g_mutex_unlock(&insn_pages_lock);

        /* Register the callback for memory access events. */
        qemu_plugin_register_vcpu_mem_cb(
            insn, vcpu_mem, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW, NULL);

        if (mode == 1)
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_insn_exec, QEMU_PLUGIN_CB_R_REGS, pinsn);
        else {
            pinsn->disas = qemu_plugin_insn_disas(insn);
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_insn_exec_assembly_print, QEMU_PLUGIN_CB_NO_REGS,
                pinsn);
        }
    }
}

static void
capture_syscall(SyscallInfo *info)
{
    SyscallHandler *handler = &syscall_handlers[info->num];

    if (handler->num != info->num) {
        g_autoptr(GString) out = g_string_new("");

        _GS_PRINTF(out, "Unhooked syscall: %" PRId64 "\n", info->num);
        qemu_plugin_outs(out->str);

        /* Initialize the handler and suppress the error message above. */
        handler->num = info->num;
        handler->n_args = 0;
        handler->syscall_ret_cb = NO_CALLBACK;
    }

    info->n_args = handler->n_args;
    for (uint16_t i = info->n_args; i < SYSCALL_MAX_ARGS; i++) {
        info->args[i] = 0;
    }
    info->n_buffers = 0; /* set in return callback */
    g_ptr_array_add(syscalls, info);
}

static void
vcpu_syscall(qemu_plugin_id_t id, unsigned int cpu_index, int64_t num,
             uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
             uint64_t a6, uint64_t a7, uint64_t a8)
{
    SyscallInfo *info = g_new0(SyscallInfo, 1);

    info->num = num;
    info->args[0] = a1;
    info->args[1] = a2;
    info->args[2] = a3;
    info->args[3] = a4;
    info->args[4] = a5;
    info->args[5] = a6;
    info->args[6] = a7;
    info->args[7] = a8;
    info->pc = last_pc;
    capture_syscall(info);
}

static void
vcpu_syscall_ret(qemu_plugin_id_t id, unsigned int cpu_index, int64_t num,
                 int64_t ret)
{
    SyscallInfo *info = g_ptr_array_index(syscalls, syscalls->len - 1);
    SyscallHandler *handler = &syscall_handlers[info->num];

    info->ret = ret;

    if (handler->syscall_ret_cb != NO_CALLBACK) {
        handler->syscall_ret_cb(info);
    }
    if (counter < breakpoint) {
        pass_syscall++;
    }
}

static void
syscall_info_free(gpointer data)
{
    SyscallInfo *info = (SyscallInfo *)data;

    if (info->n_buffers > 0) {
        for (uint16_t i = 0; i < info->n_buffers; i++) {
            g_free(info->buffers[i].buffer.data);
        }
    }
    g_free(info);
}

static void
syscall_handlers_init(void)
{
    for (size_t i = 0; i < n_syscall_handlers_installed; i++) {
        SyscallHandler *handler = &syscall_handlers_installed[i];
        syscall_handlers[handler->num] = *handler;
    }
}

static void
plugin_init(void)
{
    assert(BLOCK_SIZE <= sizeof(((Page *)0)->bytes_valid[0]) * 8);
    assert(N_PAGE_BLOCKS <= sizeof(((PageHeader *)0)->blocks_valid) * 8);
    assert(N_RISCV_GPR + N_RISCV_FP_REGS
           <= sizeof(((Registers *)0)->regs_valid) * 8);

    insn_pages = g_hash_table_new_full(NULL, g_direct_equal, NULL, g_free);
    data_pages = g_hash_table_new_full(NULL, g_direct_equal, NULL, g_free);
    registers = g_new0(Registers, 1);
    syscalls = g_ptr_array_new_with_free_func(syscall_info_free);
    syscall_handlers_init();

    if (elf_path) {
        plugin_start_addr = elf_find_start_addr(elf_path);
        plugin_start_flag = FALSE;
        g_autoptr(GString) out = g_string_new("");
        _GS_PRINTF(out, "[plugin] elf=%s start_pc=0x%016" PRIx64 "\n",
                   elf_path, plugin_start_addr);
        qemu_plugin_outs(out->str);
    }
    if (mode == 0)
        fp_assembly = fopen("assembly.data", "wb");
}

static void
syscalls_dump(void)
{
    g_autoptr(GString) out = g_string_new("");

    for (guint i = 0; i < syscalls->len; i++) {
        SyscallInfo *info = g_ptr_array_index(syscalls, i);

        _GS_PRINTF(out, "Syscall: %" PRId64 "\n", info->num);
        _GS_PRINTF(out, "  ret: %" PRId64 "\n", info->ret);
        _GS_PRINTF(out, "  nargs: %d\n", info->n_args);
        for (uint16_t j = 0; j < info->n_args; j++) {
            _GS_PRINTF(out, "  arg[%d]: 0x%lx\n", j, info->args[j]);
        }
    }

    qemu_plugin_outs(out->str);
}

static void
plugin_exit(qemu_plugin_id_t id, void *p)
{
    FILE *fp = NULL;

    GArray *insn_regions = merge_pages_to_regions(insn_pages);
    GArray *data_regions = merge_pages_to_regions(data_pages);

    if (mode == 1) {
        patch_syscall_handler(insn_regions, syscalls, pass_syscall,
                              SYSCALL_HANDLER_ADDR);

        inject_syscall_handler_page(insn_regions, SYSCALL_HANDLER_ADDR);

        inject_syscall_log(data_regions, syscalls, pass_syscall,
                           SYSCALL_LOG_BASE_ADDR);
    }

    fp = fopen("snapshot.img", "wb");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open snapshot.img\n");
        return;
    }

    write_registers(fp, registers);
    write_regions(fp, insn_regions, FLAG_INSN);
    write_regions(fp, data_regions, FLAG_DATA);
    fclose(fp);

    free_regions(insn_regions);
    insn_regions = NULL;
    free_regions(data_regions);
    data_regions = NULL;

    printf("----->Syscalls:\n");
    syscalls_dump();
    g_ptr_array_free(syscalls, TRUE);
    syscalls = NULL;

    g_free(registers);
    registers = NULL;
    g_array_free(regs_list, TRUE);
    regs_list = NULL;

    g_hash_table_destroy(insn_pages);
    insn_pages = NULL;
    g_hash_table_destroy(data_pages);
    data_pages = NULL;

    g_autoptr(GString) out = g_string_new("");
    _GS_PRINTF(out,
               "num of breakpoints: %" PRIu64 ", num of insns: %" PRIu64 "\n",
               counter, insnnum);

    if (breakpoint > counter)
        _GS_PRINTF(out, "Error: breakpoint invalid!\n");
    qemu_plugin_outs(out->str);

    if (mode == 0) {
        fprintf(fp_assembly, "%lu", counter);
        fflush(fp_assembly);
        fclose(fp_assembly);
    }
}

static void
parse_args(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];

        if (g_str_has_prefix(a, "elf=")) {
            const char *v = a + strlen("elf=");
            if (v[0] == '\0') {
                qemu_plugin_outs("[ERR] elf= is empty\n");
                exit(1);
            }
            g_free(elf_path);
            elf_path = g_strdup(v);
            continue;
        }

        if (g_str_has_prefix(a, "k=")) {
            const char *v = a + strlen("k=");
            int64_t x = g_ascii_strtoll(v, NULL, 10);
            if (x >= 0)
                breakpoint = (uint64_t)x;
            else {
                g_autoptr(GString) out = g_string_new("");
                _GS_PRINTF(out, "breakpoint < 0\n");
                qemu_plugin_outs(out->str);
                exit(0);
            }
            continue;
        }

        if (g_str_has_prefix(a, "mode=")) {
            const char *v = a + strlen("mode=");
            if (!strcmp(v, "a") || !strcmp(v, "assembly") || !strcmp(v, "0")) {
                mode = 0;
            } else if (!strcmp(v, "s") || !strcmp(v, "save")
                       || !strcmp(v, "1")) {
                mode = 1;
            } else {
                print_debug("Usage: mode = save(s)/assembly(a)\n");
                exit(0);
            }
        }
    }
}

QEMU_PLUGIN_EXPORT int
qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc,
                    char **argv)
{
    parse_args(argc, argv);
    plugin_init();

    qemu_plugin_register_vcpu_init_cb(id, vcpu_init);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_vcpu_syscall_cb(id, vcpu_syscall);
    qemu_plugin_register_vcpu_syscall_ret_cb(id, vcpu_syscall_ret);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}
