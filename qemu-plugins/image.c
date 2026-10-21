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
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>

#include "image.h"

#define TRAMP_NORMAL_OFF 0x00

/*
 * Merge regions
 */
static gint
compare_pages(gconstpointer a, gconstpointer b)
{
    Page *pa = (Page *)a;
    Page *pb = (Page *)b;
    assert(pa->vpn != pb->vpn);
    return pa->vpn < pb->vpn ? -1 : 1;
}

static void
add_region_to_regions(GArray *regions, Page *page)
{
    Region *region;
    guint index = regions->len;

    g_array_set_size(regions, index + 1);
    region = &g_array_index(regions, Region, index);
    region->vpn_start = page->vpn;
    region->vpn_end = page->vpn;
    region->pages = g_ptr_array_new();
    g_ptr_array_add(region->pages, page);
}

static void
add_page_to_regions(gpointer data, gpointer user_data)
{
    Region *region;
    Page *page = (Page *)data;
    GArray *regions = (GArray *)user_data;

    if (regions->len == 0) {
        add_region_to_regions(regions, page);
        return;
    }

    region = &g_array_index(regions, Region, regions->len - 1);
    assert(page->vpn > region->vpn_end);
    assert(region->pages != NULL);
    if (page->vpn <= region->vpn_end + REGION_MAX_GAP) {
        region->vpn_end = page->vpn;
        g_ptr_array_add(region->pages, page);
    } else {
        add_region_to_regions(regions, page);
    }
}

GArray *
merge_pages_to_regions(GHashTable *hash_pages)
{
    assert(hash_pages != NULL);

    /* GArray: not zero-terminated, clear on allocation */
    GArray *regions = g_array_new(FALSE, TRUE, sizeof(Region));
    GList *pages = g_hash_table_get_values(hash_pages);

    pages = g_list_sort(pages, compare_pages);
    g_list_foreach(pages, add_page_to_regions, regions);
    return regions;
}

/*
 * Write regions
 */
static uint64_t
page_blocks_valid_flag(Page *page)
{
    uint64_t flag = 0;

    for (int i = 0; i < N_PAGE_BLOCKS; i++) {
        flag |= (uint64_t)(page->bytes_valid[i] ? 1 : 0) << i;
    }
    return flag;
}

static void
write_page(gpointer data, gpointer user_data)
{
    Page *page = (Page *)data;
    FILE *fp = (FILE *)user_data;
    PageHeader header;

    header.common.magic = MAGIC_PAGE;
    header.common.size = sizeof(PageHeader);
    header.vpn = page->vpn;
    header.blocks_valid = page_blocks_valid_flag(page);

    fwrite(&header, sizeof(header), 1, fp);
    print_debug("Page:0x%lx\n", page->vpn);
    for (int i = 0; i < N_PAGE_BLOCKS; i++) {
        if (page->bytes_valid[i]) {
            fwrite(&page->data[i * BLOCK_SIZE], BLOCK_SIZE, 1, fp);
        }
    }
}

static void
write_region(FILE *fp, Region *region)
{
    RegionHeader header;

    header.common.magic = MAGIC_REGION;
    header.common.size = sizeof(RegionHeader);
    header.vpn_start = region->vpn_start;
    header.vpn_end = region->vpn_end;
    header.n_pages = region->pages->len;

    fwrite(&header, sizeof(header), 1, fp);
    for (guint i = 0; i < region->pages->len; i++) {
        Page *page = g_ptr_array_index(region->pages, i);
        write_page(page, fp);
    }
}

void
write_regions(FILE *fp, GArray *regions, uint32_t flags)
{
    RegionListHeader header;

    assert(fp != NULL && regions != NULL);

    header.common.magic = MAGIC_REGION_LIST;
    header.common.size = sizeof(RegionListHeader);
    header.n_regions = regions->len;
    header.flags = flags;

    fwrite(&header, sizeof(header), 1, fp);
    for (guint i = 0; i < regions->len; i++) {
        Region *region = &g_array_index(regions, Region, i);
        write_region(fp, region);
    }
}

/*
 * Free regions
 */
void
free_regions(GArray *regions)
{
    assert(regions != NULL);

    for (guint i = 0; i < regions->len; i++) {
        Region *region = &g_array_index(regions, Region, i);
        g_ptr_array_free(region->pages, TRUE);
    }
    g_array_free(regions, TRUE);
}

/*
 * Write registers
 */
void
write_registers(FILE *fp, Registers *registers)
{
    RegistersHeader header;

    assert(fp != NULL && registers != NULL);

    header.common.magic = MAGIC_REGISTERS;
    header.common.size = sizeof(RegistersHeader);
    header.pc = registers->pc;
    header.regs_valid = registers->regs_valid;
    header.csr_regs_valid = registers->csr_regs_valid;

    fwrite(&header, sizeof(header), 1, fp);
    for (int i = 0; i < N_RISCV_GPR + N_RISCV_FP_REGS; i++) {
        if ((registers->regs_valid >> i) & 1) {
            fwrite(&registers->regs[i], sizeof(registers->regs[i]), 1, fp);
        }
    }
    for (int i = 0; i < N_RISCV_CSR; i++) {
        if ((registers->csr_regs_valid >> i) & 1) {
            fwrite(&registers->csr_regs[i], sizeof(registers->csr_regs[i]), 1,
                   fp);
        }
    }
}

/*
 * Tool functions of writing pages & regions
 */
static Page *
region_find_page(Region *region, uint64_t vpn)
{
    assert(region != NULL);

    for (guint i = 0; i < region->pages->len; i++) {
        Page *page = g_ptr_array_index(region->pages, i);
        if (page->vpn == vpn) {
            return page;
        }
    }
    return NULL;
}

static Region *
find_region_by_vpn(GArray *regions, uint64_t vpn)
{
    assert(regions != NULL);

    for (guint i = 0; i < regions->len; i++) {
        Region *region = &g_array_index(regions, Region, i);

        if (vpn < region->vpn_start || vpn > region->vpn_end) {
            continue;
        }
        if (region_find_page(region, vpn) != NULL) {
            return region;
        }
    }
    return NULL;
}

static void
page_write_bytes(Page *page, uint64_t vaddr, const uint8_t *src, size_t size)
{
    size_t off = PAGE_OFFSET(vaddr);

    assert(page != NULL);
    assert(src != NULL);
    assert(off + size <= PAGE_SIZE);

    for (size_t i = 0; i < size; i++) {
        size_t pos = off + i;
        page->data[pos] = src[i];
        page->bytes_valid[BLOCK_INDEX(pos)] |= (1ULL << BLOCK_POS(pos));
    }
}

static void
page_write_u32(Page *page, uint64_t vaddr, uint32_t insn)
{
    uint8_t raw[4];

    raw[0] = (uint8_t)(insn & 0xff);
    raw[1] = (uint8_t)((insn >> 8) & 0xff);
    raw[2] = (uint8_t)((insn >> 16) & 0xff);
    raw[3] = (uint8_t)((insn >> 24) & 0xff);

    page_write_bytes(page, vaddr, raw, sizeof(raw));
}

static uint32_t
page_read_u32(Page *page, uint64_t vaddr)
{
    size_t off = PAGE_OFFSET(vaddr);

    assert(page != NULL);
    assert(off + 4 <= PAGE_SIZE);

    return ((uint32_t)page->data[off + 0])
           | ((uint32_t)page->data[off + 1] << 8)
           | ((uint32_t)page->data[off + 2] << 16)
           | ((uint32_t)page->data[off + 3] << 24);
}

static uint64_t
align_up_u64(uint64_t x, uint64_t a)
{
    assert(a != 0 && (a & (a - 1)) == 0);
    return (x + a - 1) & ~(a - 1);
}

static void
store_u64_le(uint8_t out[8], uint64_t v)
{
    out[0] = (uint8_t)(v & 0xff);
    out[1] = (uint8_t)((v >> 8) & 0xff);
    out[2] = (uint8_t)((v >> 16) & 0xff);
    out[3] = (uint8_t)((v >> 24) & 0xff);
    out[4] = (uint8_t)((v >> 32) & 0xff);
    out[5] = (uint8_t)((v >> 40) & 0xff);
    out[6] = (uint8_t)((v >> 48) & 0xff);
    out[7] = (uint8_t)((v >> 56) & 0xff);
}

static void
region_write_bytes(Region *region, uint64_t vaddr, const uint8_t *src,
                   size_t size)
{
    while (size > 0) {
        uint64_t vpn = PAGE_NUMBER(vaddr);
        Page *page = region_find_page(region, vpn);
        size_t off = PAGE_OFFSET(vaddr);
        size_t chunk;

        assert(page != NULL);

        chunk = PAGE_SIZE - off;
        if (chunk > size) {
            chunk = size;
        }

        page_write_bytes(page, vaddr, src, chunk);

        vaddr += chunk;
        src += chunk;
        size -= chunk;
    }
}

static void
region_write_u64(Region *region, uint64_t vaddr, uint64_t value)
{
    uint8_t raw[8];
    store_u64_le(raw, value);
    region_write_bytes(region, vaddr, raw, sizeof(raw));
}

static gint
compare_regions_by_vpn_start(gconstpointer a, gconstpointer b)
{
    const Region *ra = (const Region *)a;
    const Region *rb = (const Region *)b;

    if (ra->vpn_start < rb->vpn_start)
        return -1;
    if (ra->vpn_start > rb->vpn_start)
        return 1;
    return 0;
}

static int
is_exit_syscall(int64_t num)
{
    return num == 93 || num == 94;
}

/*
 * instruction encoders - RISC-V asm <-> C
 */
static uint32_t
encode_auipc(uint32_t rd, int32_t imm20)
{
    return (((uint32_t)imm20 & 0xfffffU) << 12) | ((rd & 0x1fU) << 7) | 0x17U;
}

static uint32_t
encode_jal(uint32_t rd, uint64_t pc, uint64_t target)
{
    int64_t delta = (int64_t)target - (int64_t)pc;
    uint32_t u;

    assert((delta & 1) == 0);
    assert(delta >= -1048576 && delta <= 1048574);

    u = (uint32_t)delta;

    return (((u >> 20) & 0x1U) << 31) | (((u >> 1) & 0x3ffU) << 21)
           | (((u >> 11) & 0x1U) << 20) | (((u >> 12) & 0xffU) << 12)
           | ((rd & 0x1fU) << 7) | 0x6fU;
}

static uint32_t
encode_jalr(uint32_t rd, uint32_t rs1, int32_t imm12)
{
    assert(imm12 >= -2048 && imm12 <= 2047);

    return ((((uint32_t)imm12) & 0xfffU) << 20) | ((rs1 & 0x1fU) << 15)
           | (0U << 12) | ((rd & 0x1fU) << 7) | 0x67U;
}

static uint32_t
encode_op_imm(uint32_t funct3, uint32_t rd, uint32_t rs1, int32_t imm12)
{
    assert(imm12 >= -2048 && imm12 <= 2047);

    return ((((uint32_t)imm12) & 0xfffU) << 20) | ((rs1 & 0x1fU) << 15)
           | ((funct3 & 0x7U) << 12) | ((rd & 0x1fU) << 7) | 0x13U;
}

static uint32_t
encode_load(uint32_t funct3, uint32_t rd, uint32_t rs1, int32_t imm12)
{
    assert(imm12 >= -2048 && imm12 <= 2047);

    return ((((uint32_t)imm12) & 0xfffU) << 20) | ((rs1 & 0x1fU) << 15)
           | ((funct3 & 0x7U) << 12) | ((rd & 0x1fU) << 7) | 0x03U;
}

static uint32_t
encode_store(uint32_t funct3, uint32_t rs2, uint32_t rs1, int32_t imm12)
{
    uint32_t imm = ((uint32_t)imm12) & 0xfffU;

    assert(imm12 >= -2048 && imm12 <= 2047);

    return (((imm >> 5) & 0x7fU) << 25) | ((rs2 & 0x1fU) << 20)
           | ((rs1 & 0x1fU) << 15) | ((funct3 & 0x7U) << 12)
           | ((imm & 0x1fU) << 7) | 0x23U;
}

static uint32_t
encode_branch(uint32_t funct3, uint32_t rs1, uint32_t rs2, uint64_t pc,
              uint64_t target)
{
    int64_t delta = (int64_t)target - (int64_t)pc;
    uint32_t u;

    /* B-type range: [-4096, +4094], aligned by 2 */
    assert((delta & 1) == 0);
    assert(delta >= -4096 && delta <= 4094);

    u = (uint32_t)delta;

    return (((u >> 12) & 0x1U) << 31) | (((u >> 5) & 0x3fU) << 25)
           | ((rs2 & 0x1fU) << 20) | ((rs1 & 0x1fU) << 15)
           | ((funct3 & 0x7U) << 12) | (((u >> 1) & 0xfU) << 8)
           | (((u >> 11) & 0x1U) << 7) | 0x63U;
}

static uint32_t
encode_op(uint32_t funct7, uint32_t funct3, uint32_t rd, uint32_t rs1,
          uint32_t rs2)
{
    return ((funct7 & 0x7fU) << 25) | ((rs2 & 0x1fU) << 20)
           | ((rs1 & 0x1fU) << 15) | ((funct3 & 0x7U) << 12)
           | ((rd & 0x1fU) << 7) | 0x33U;
}

static uint32_t
encode_ori(uint32_t rd, uint32_t rs1, int32_t imm12)
{
    return encode_op_imm(0x6U, rd, rs1, imm12);
}

static uint32_t
encode_add(uint32_t rd, uint32_t rs1, uint32_t rs2)
{
    return encode_op(0x00U, 0x0U, rd, rs1, rs2);
}

static uint32_t
encode_addi(uint32_t rd, uint32_t rs1, int32_t imm12)
{
    return encode_op_imm(0x0U, rd, rs1, imm12);
}

static uint32_t
encode_andi(uint32_t rd, uint32_t rs1, int32_t imm12)
{
    return encode_op_imm(0x7U, rd, rs1, imm12);
}

static uint32_t
encode_ld(uint32_t rd, uint32_t rs1, int32_t imm12)
{
    return encode_load(0x3U, rd, rs1, imm12);
}

static uint32_t
encode_lbu(uint32_t rd, uint32_t rs1, int32_t imm12)
{
    return encode_load(0x4U, rd, rs1, imm12);
}

static uint32_t
encode_sd(uint32_t rs2, uint32_t rs1, int32_t imm12)
{
    return encode_store(0x3U, rs2, rs1, imm12);
}

static uint32_t
encode_sb(uint32_t rs2, uint32_t rs1, int32_t imm12)
{
    return encode_store(0x0U, rs2, rs1, imm12);
}

static uint32_t
encode_beq(uint32_t rs1, uint32_t rs2, uint64_t pc, uint64_t target)
{
    return encode_branch(0x0U, rs1, rs2, pc, target);
}

static uint32_t
encode_bne(uint32_t rs1, uint32_t rs2, uint64_t pc, uint64_t target)
{
    return encode_branch(0x1U, rs1, rs2, pc, target);
}

static uint32_t
encode_bgeu(uint32_t rs1, uint32_t rs2, uint64_t pc, uint64_t target)
{
    return encode_branch(0x7U, rs1, rs2, pc, target);
}

static uint32_t
encode_mv(uint32_t rd, uint32_t rs)
{
    return encode_addi(rd, rs, 0);
}

static uint32_t
encode_jr(uint32_t rs)
{
    return encode_jalr(0, rs, 0);
}

/*
 * Tiny emitter - write syscall handler into pages
 */
static void
emit_init(InsnEmitter *e, Page *page, uint64_t base_addr)
{
    assert(e != NULL && page != NULL);
    e->page = page;
    e->base = base_addr;
    e->pc = base_addr;
}

static uint64_t
emit_here(InsnEmitter *e)
{
    return e->pc;
}

static void
emit_u32(InsnEmitter *e, uint32_t insn)
{
    page_write_u32(e->page, e->pc, insn);
    e->pc += 4;
}

static void
emit_addi(InsnEmitter *e, uint32_t rd, uint32_t rs1, int32_t imm12)
{
    emit_u32(e, encode_addi(rd, rs1, imm12));
}

static void
emit_andi(InsnEmitter *e, uint32_t rd, uint32_t rs1, int32_t imm12)
{
    emit_u32(e, encode_andi(rd, rs1, imm12));
}

static void
emit_ld(InsnEmitter *e, uint32_t rd, uint32_t rs1, int32_t imm12)
{
    emit_u32(e, encode_ld(rd, rs1, imm12));
}

static void
emit_lbu(InsnEmitter *e, uint32_t rd, uint32_t rs1, int32_t imm12)
{
    emit_u32(e, encode_lbu(rd, rs1, imm12));
}

static void
emit_sd(InsnEmitter *e, uint32_t rs2, uint32_t rs1, int32_t imm12)
{
    emit_u32(e, encode_sd(rs2, rs1, imm12));
}

static void
emit_sb(InsnEmitter *e, uint32_t rs2, uint32_t rs1, int32_t imm12)
{
    emit_u32(e, encode_sb(rs2, rs1, imm12));
}

static void
emit_mv(InsnEmitter *e, uint32_t rd, uint32_t rs)
{
    emit_u32(e, encode_mv(rd, rs));
}

static void
emit_jr(InsnEmitter *e, uint32_t rs)
{
    emit_u32(e, encode_jr(rs));
}

static void
emit_j(InsnEmitter *e, uint64_t target)
{
    emit_u32(e, encode_jal(0, e->pc, target));
}

static void
emit_bne(InsnEmitter *e, uint32_t rs1, uint32_t rs2, uint64_t target)
{
    emit_u32(e, encode_bne(rs1, rs2, e->pc, target));
}

static void
emit_ori(InsnEmitter *e, uint32_t rd, uint32_t rs1, int32_t imm12)
{
    emit_u32(e, encode_ori(rd, rs1, imm12));
}

static void
emit_add(InsnEmitter *e, uint32_t rd, uint32_t rs1, uint32_t rs2)
{
    emit_u32(e, encode_add(rd, rs1, rs2));
}

static void
emit_load_abs64_via_literal(InsnEmitter *e, uint32_t rd, uint64_t literal_addr)
{
    int64_t delta;
    int32_t hi20, lo12;

    delta = (int64_t)literal_addr - (int64_t)e->pc;
    assert(delta >= INT32_MIN && delta <= INT32_MAX);

    hi20 = (int32_t)((delta + 0x800) >> 12);
    lo12 = (int32_t)(delta - ((int64_t)hi20 << 12));
    assert(lo12 >= -2048 && lo12 <= 2047);

    emit_u32(e, encode_auipc(rd, hi20));
    emit_u32(e, encode_ld(rd, rd, lo12));
}

static void
emit_far_jump_at(Page *page, uint64_t stub_addr, uint32_t scratch_reg,
                 uint64_t target_addr)
{
    int64_t delta = (int64_t)target_addr - (int64_t)stub_addr;
    int32_t hi20, lo12;
    uint32_t insn0, insn1;

    assert(delta >= INT32_MIN && delta <= INT32_MAX);

    hi20 = (int32_t)((delta + 0x800) >> 12);
    lo12 = (int32_t)(delta - ((int64_t)hi20 << 12));
    assert(lo12 >= -2048 && lo12 <= 2047);

    insn0 = encode_auipc(scratch_reg, hi20);
    insn1 = encode_jalr(0, scratch_reg, lo12);

    page_write_u32(page, stub_addr + 0, insn0);
    page_write_u32(page, stub_addr + 4, insn1);
}

/*
 * Syscall replay handler
 */
static void
emit_replay_handler(Page *page, uint64_t handler_addr)
{
    InsnEmitter e;

    uint64_t br_no_log_pc;
    uint64_t br_no_buf_pc;
    uint64_t br_copy_done_pc;
    uint64_t j_restore_pc;

    uint64_t label_buf_loop;
    uint64_t label_copy_loop;
    uint64_t label_copy_done;
    uint64_t label_done;
    uint64_t label_no_log;
    uint64_t label_restore;

    /* 把 literal 放在这一页靠后的位置 */
    uint64_t lit_syslog_base = handler_addr + 0x3f0;

    assert(page != NULL);
    assert(PAGE_OFFSET(handler_addr) == 0);

    emit_init(&e, page, handler_addr);

    /* a7 = *(u64 *)lit_syslog_base */
    emit_load_abs64_via_literal(&e, X_A7, lit_syslog_base);

    /* retaddr_slot = a0 */
    emit_sd(&e, X_A0, X_A7, RETADDR_SLOT);

    /* save live regs */
    emit_sd(&e, X_T0, X_A7, SAVE_T0);
    emit_sd(&e, X_T1, X_A7, SAVE_T1);
    emit_sd(&e, X_T2, X_A7, SAVE_T2);
    emit_sd(&e, X_T3, X_A7, SAVE_T3);
    emit_sd(&e, X_T4, X_A7, SAVE_T4);
    emit_sd(&e, X_T5, X_A7, SAVE_T5);
    emit_sd(&e, X_T6, X_A7, SAVE_T6);
    emit_sd(&e, X_A1, X_A7, SAVE_A1);
    emit_sd(&e, X_A2, X_A7, SAVE_A2);
    emit_sd(&e, X_A3, X_A7, SAVE_A3);
    emit_sd(&e, X_A4, X_A7, SAVE_A4);
    emit_sd(&e, X_A5, X_A7, SAVE_A5);
    emit_sd(&e, X_A6, X_A7, SAVE_A6);

    emit_ld(&e, X_T0, X_A7, LOG_CURSOR);
    emit_ld(&e, X_T1, X_A7, LOG_END);

    br_no_log_pc = emit_here(&e);
    emit_u32(&e, 0);

    emit_ld(&e, X_T2, X_T0, 0);
    emit_ld(&e, X_T3, X_T0, 8);
    emit_ld(&e, X_T4, X_T0, 16);
    emit_addi(&e, X_T0, X_T0, 24);

    br_no_buf_pc = emit_here(&e);
    emit_u32(&e, 0);

    label_buf_loop = emit_here(&e);

    emit_ld(&e, X_T1, X_T0, 0);
    emit_ld(&e, X_T2, X_T0, 8);
    emit_addi(&e, X_T0, X_T0, 16);

    label_copy_loop = emit_here(&e);

    br_copy_done_pc = emit_here(&e);
    emit_u32(&e, 0);

    emit_lbu(&e, X_T5, X_T0, 0);
    emit_sb(&e, X_T5, X_T1, 0);
    emit_addi(&e, X_T0, X_T0, 1);
    emit_addi(&e, X_T1, X_T1, 1);
    emit_addi(&e, X_T2, X_T2, -1);
    emit_j(&e, label_copy_loop);

    label_copy_done = emit_here(&e);

    emit_addi(&e, X_T0, X_T0, 7);
    emit_andi(&e, X_T0, X_T0, -8);

    emit_addi(&e, X_T4, X_T4, -1);
    emit_bne(&e, X_T4, X_ZERO, label_buf_loop);

    label_done = emit_here(&e);

    emit_sd(&e, X_T0, X_A7, LOG_CURSOR);
    emit_mv(&e, X_A0, X_T3);

    j_restore_pc = emit_here(&e);
    emit_u32(&e, 0);

    label_no_log = emit_here(&e);
    emit_addi(&e, X_A0, X_ZERO, -38);

    label_restore = emit_here(&e);

    emit_ld(&e, X_T0, X_A7, SAVE_T0);
    emit_ld(&e, X_T1, X_A7, SAVE_T1);
    emit_ld(&e, X_T2, X_A7, SAVE_T2);
    emit_ld(&e, X_T3, X_A7, SAVE_T3);
    emit_ld(&e, X_T4, X_A7, SAVE_T4);
    emit_ld(&e, X_T5, X_A7, SAVE_T5);
    emit_ld(&e, X_T6, X_A7, SAVE_T6);
    emit_ld(&e, X_A1, X_A7, SAVE_A1);
    emit_ld(&e, X_A2, X_A7, SAVE_A2);
    emit_ld(&e, X_A3, X_A7, SAVE_A3);
    emit_ld(&e, X_A4, X_A7, SAVE_A4);
    emit_ld(&e, X_A5, X_A7, SAVE_A5);
    emit_ld(&e, X_A6, X_A7, SAVE_A6);

    emit_ld(&e, X_A7, X_A7, RETADDR_SLOT);
    emit_jr(&e, X_A7);

    /* patch forward branches/jumps */
    page_write_u32(page, br_no_log_pc,
                   encode_bgeu(X_T0, X_T1, br_no_log_pc, label_no_log));

    page_write_u32(page, br_no_buf_pc,
                   encode_beq(X_T4, X_ZERO, br_no_buf_pc, label_done));

    page_write_u32(page, br_copy_done_pc,
                   encode_beq(X_T2, X_ZERO, br_copy_done_pc, label_copy_done));

    page_write_u32(page, j_restore_pc,
                   encode_jal(X_ZERO, j_restore_pc, label_restore));

    uint8_t raw_syslog_base[8];
    store_u64_le(raw_syslog_base, SYSCALL_LOG_BASE_ADDR);
    page_write_bytes(page, lit_syslog_base, raw_syslog_base, 8);

    assert((e.pc - handler_addr) < 0x3f0);
}

/*
 * Insert syscall handler into insn_regions
 */
void
inject_syscall_handler_page(GArray *insn_regions, uint64_t handler_addr)
{
    uint64_t vpn = PAGE_NUMBER(handler_addr);
    Page *page;
    Region region;

    assert(insn_regions != NULL);
    assert(PAGE_OFFSET(handler_addr) == 0);

    for (guint i = 0; i < insn_regions->len; i++) {
        Region *r = &g_array_index(insn_regions, Region, i);

        if (region_find_page(r, vpn) != NULL) {
            // print_debug("[syscall-handler] conflict: vpn=0x%lx already
            // exists\n", vpn);
            assert(0 && "handler page conflict");
        }

        if (vpn >= r->vpn_start && vpn <= r->vpn_end) {
            // print_debug("[syscall-handler] conflict: vpn=0x%lx lies inside
            // existing region [0x%lx, 0x%lx]\n",
            //             vpn, r->vpn_start, r->vpn_end);
            assert(0 && "handler vpn overlaps existing region span");
        }
    }

    page = g_new0(Page, 1);
    page->vpn = vpn;

    /* syscall handler */
    emit_replay_handler(page, handler_addr);

    region.vpn_start = vpn;
    region.vpn_end = vpn;
    region.pages = g_ptr_array_new();
    g_ptr_array_add(region.pages, page);

    g_array_append_val(insn_regions, region);
    g_array_sort(insn_regions, compare_regions_by_vpn_start);

    print_debug(
        "[syscall-handler] add handler page @vpn=0x%lx, normal=0x%lx\n", vpn,
        handler_addr);
}

/*
 * Build a far jump stub at trampoline page start:
 *   auipc a7, %pcrel_hi(replay_handler)
 *   jalr  x0, %pcrel_lo(replay_handler)(a7)
 *   trampoline page: the next page of each insn region
 */
static void
emit_region_trampoline(Page *tramp_page, uint64_t replay_handler_addr)
{
    uint64_t tramp_pc;

    assert(tramp_page != NULL);

    tramp_pc = tramp_page->vpn << PAGE_SHIFT;

    /* a0 save return pc，trampoline use a7 to jump */
    emit_far_jump_at(tramp_page, tramp_pc + TRAMP_NORMAL_OFF, X_A7,
                     replay_handler_addr);
}

static Page *
ensure_region_trampoline(Region *region, GHashTable *region2tramp,
                         uint64_t replay_handler_addr)
{
    Page *tramp_page;
    uint64_t tramp_vpn;

    assert(region != NULL);
    assert(region2tramp != NULL);

    tramp_page = g_hash_table_lookup(region2tramp, region);
    if (tramp_page != NULL) {
        return tramp_page;
    }

    tramp_vpn = region->vpn_end + 1;
    assert(region_find_page(region, tramp_vpn) == NULL);

    tramp_page = g_new0(Page, 1);
    tramp_page->vpn = tramp_vpn;

    emit_region_trampoline(tramp_page, replay_handler_addr);

    g_ptr_array_add(region->pages, tramp_page);
    region->vpn_end = tramp_vpn;

    g_hash_table_insert(region2tramp, region, tramp_page);

    // print_debug("[inline-replay] region [0x%lx, 0x%lx] add tramp page
    // vpn=0x%lx\n",
    //             region->vpn_start, region->vpn_end, tramp_vpn);

    return tramp_page;
}

/*
 * Insert trampoline pages and replace ecall
 */
void
patch_syscall_handler(GArray *insn_regions, GPtrArray *syscalls,
                      uint32_t pass_syscall, uint64_t syscall_handler_addr)
{
    GHashTable *region2tramp;
    guint patched = 0;

    assert(insn_regions != NULL);
    assert(syscalls != NULL);
    assert((guint)pass_syscall <= syscalls->len);
    assert((syscall_handler_addr & 0x3) == 0);

    region2tramp = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (guint i = (guint)pass_syscall; i < syscalls->len; i++) {
        SyscallInfo *info = g_ptr_array_index(syscalls, i);
        uint64_t pc, vpn;
        uint64_t tramp_base;
        uint64_t normal_tramp_pc;
        Region *region;
        Page *code_page, *tramp_page;
        uint32_t old_insn, new_insn;

        assert(info != NULL);

        /* exit / exit_group */
        if (is_exit_syscall(info->num)) {
            // print_debug("[patcher] keep exit ecall @0x%lx unchanged\n",
            // info->pc);
            continue;
        }

        pc = info->pc;
        vpn = PAGE_NUMBER(pc);

        region = find_region_by_vpn(insn_regions, vpn);
        assert(region != NULL);

        code_page = region_find_page(region, vpn);
        assert(code_page != NULL);

        tramp_page = ensure_region_trampoline(region, region2tramp,
                                              syscall_handler_addr);
        tramp_base = tramp_page->vpn << PAGE_SHIFT;

        old_insn = page_read_u32(code_page, pc);

        normal_tramp_pc = tramp_base + TRAMP_NORMAL_OFF;
        new_insn = encode_jal(X_A0, pc,
                              normal_tramp_pc); /* jal a0, normal_tramp */

        // print_debug("[patcher] patch normal ecall @0x%lx -> jal a0,
        // 0x%lx\n",
        //             pc, normal_tramp_pc);

        if (old_insn == 0x00000073U) {
            page_write_u32(code_page, pc, new_insn);
            patched++;
        } else if (old_insn == new_insn) {
            continue;
        } else {
            // print_debug("[patcher] unexpected insn @0x%lx = 0x%08x\n",
            //             pc, old_insn);
            assert(0 && "inline replay patch conflict");
        }
    }

    // print_debug("[patcher] patched syscall sites: %u\n", patched);

    g_hash_table_destroy(region2tramp);
}

/* runtime syscall log injection
 * layout:
 *   BASE + 0x0000 : u64 cursor
 *   BASE + 0x0008 : u64 start
 *   BASE + 0x0010 : u64 end
 *   BASE + 0x0018 : u64 retaddr_slot
 *   BASE + 0x0020 : scratch area (zero-initialized)
 *   BASE + 0x0100 : syscall records start
 *
 * record format (same as make_entry runtime layout):
 *   u64 sysno
 *   u64 retval
 *   u64 n_buffers
 *   repeat n_buffers:
 *     u64 addr
 *     u64 size
 *     u8  raw[size]
 *     align 8
 */
static uint64_t
calc_runtime_syscall_log_size(GPtrArray *syscalls, uint32_t pass_syscall)
{
    uint64_t size = SYSCALL_LOG_FIXED_SIZE;

    assert(syscalls != NULL);
    assert((guint)pass_syscall <= syscalls->len);

    for (guint i = (guint)pass_syscall; i < syscalls->len; i++) {
        SyscallInfo *info = g_ptr_array_index(syscalls, i);

        assert(info != NULL);

        /* exit / exit_group 不放进 runtime replay log */
        if (is_exit_syscall(info->num)) {
            continue;
        }

        /* sysno + retval + n_buffers */
        size += 8 + 8 + 8;

        for (guint j = 0; j < info->n_buffers; j++) {
            SyscallBuffer *buf = &info->buffers[j];

            /* addr + size + raw bytes + align8 */
            size += 8 + 8;
            size += (uint64_t)buf->buffer.len;
            size = align_up_u64(size, 8);
        }
    }

    return size;
}
static Region
build_runtime_log_region(uint64_t base_addr, uint64_t total_size)
{
    Region region;
    uint64_t start_vpn, end_vpn;

    assert(PAGE_OFFSET(base_addr) == 0);
    assert(total_size > 0);

    start_vpn = PAGE_NUMBER(base_addr);
    end_vpn = PAGE_NUMBER(base_addr + total_size - 1);

    region.vpn_start = start_vpn;
    region.vpn_end = end_vpn;
    region.pages = g_ptr_array_new();

    for (uint64_t vpn = start_vpn; vpn <= end_vpn; vpn++) {
        Page *page = g_new0(Page, 1);
        page->vpn = vpn;
        g_ptr_array_add(region.pages, page);
    }

    return region;
}

static void
check_runtime_log_region_conflict(GArray *data_regions, uint64_t base_addr,
                                  uint64_t total_size)
{
    uint64_t new_start_vpn, new_end_vpn;

    assert(data_regions != NULL);
    assert(total_size > 0);

    new_start_vpn = PAGE_NUMBER(base_addr);
    new_end_vpn = PAGE_NUMBER(base_addr + total_size - 1);

    for (guint i = 0; i < data_regions->len; i++) {
        Region *r = &g_array_index(data_regions, Region, i);

        /* region span overlap */
        if (!(new_end_vpn < r->vpn_start || new_start_vpn > r->vpn_end)) {
            // print_debug("[syscall-log] conflict: new region [0x%lx, 0x%lx]
            // overlaps existing data region [0x%lx, 0x%lx]\n",
            //             new_start_vpn, new_end_vpn, r->vpn_start,
            //             r->vpn_end);
            assert(0 && "syscall log region conflict");
        }
    }
}

void
inject_syscall_log(GArray *data_regions, GPtrArray *syscalls,
                   uint32_t pass_syscall, uint64_t base_addr)
{
    uint64_t total_size;
    uint64_t records_start;
    uint64_t cur;
    uint64_t end_addr;
    guint injected_syscalls = 0;
    Region region;

    assert(data_regions != NULL);
    assert(syscalls != NULL);
    assert((guint)pass_syscall <= syscalls->len);
    assert(PAGE_OFFSET(base_addr) == 0);

    total_size = calc_runtime_syscall_log_size(syscalls, pass_syscall);
    records_start = base_addr + SYSCALL_LOG_FIXED_SIZE;

    check_runtime_log_region_conflict(data_regions, base_addr, total_size);

    region = build_runtime_log_region(base_addr, total_size);

    cur = records_start;

    for (guint i = (guint)pass_syscall; i < syscalls->len; i++) {
        SyscallInfo *info = g_ptr_array_index(syscalls, i);

        assert(info != NULL);

        /* exit / exit_group 不写入 runtime replay log */
        if (is_exit_syscall(info->num)) {
            continue;
        }

        region_write_u64(&region, cur + 0, (uint64_t)info->num);
        region_write_u64(&region, cur + 8, (uint64_t)info->ret);
        region_write_u64(&region, cur + 16, (uint64_t)info->n_buffers);
        cur += 24;
        injected_syscalls++;

        for (guint j = 0; j < info->n_buffers; j++) {
            SyscallBuffer *buf = &info->buffers[j];
            uint64_t size = (uint64_t)buf->buffer.len;

            region_write_u64(&region, cur + 0, buf->addr);
            region_write_u64(&region, cur + 8, size);
            cur += 16;

            if (size > 0) {
                region_write_bytes(&region, cur, buf->buffer.data,
                                   (size_t)size);
                cur += size;
            }

            cur = align_up_u64(cur, 8);
        }
    }

    end_addr = cur;

    /* fill fixed metadata */
    region_write_u64(&region, base_addr + LOG_CURSOR, records_start);
    region_write_u64(&region, base_addr + LOG_START, records_start);
    region_write_u64(&region, base_addr + LOG_END, end_addr);
    region_write_u64(&region, base_addr + RETADDR_SLOT, 0);

    /* append as one standalone data region */
    g_array_append_val(data_regions, region);
    g_array_sort(data_regions, compare_regions_by_vpn_start);

    // print_debug("[syscall-log] injected @0x%lx, records_start=0x%lx,
    // end=0x%lx, size=0x%lx, n_syscalls=%u\n",
    //             base_addr, records_start, end_addr, total_size,
    //             injected_syscalls);
}
