/*
 * RunShot: Live program state capture and playback tool
 *
 * Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 */

#include <stdint.h>
#include <stdio.h>

#include "snapshot.h"
#include "xutils.h"

/*
 * Region structures
 */
typedef struct
{
    uint64_t vpn_start;
    uint64_t vpn_end;
    uint32_t n_pages;
    Page *pages[0];
} Region;

typedef struct
{
    uint32_t n_regions;
    uint32_t flags;
    Region *regions[0];
} RegionList;

static Page *
read_page(FILE *fp)
{
    PageHeader header;

    fread(&header, sizeof(header), 1, fp);
    ASSERT(header.common.magic == MAGIC_PAGE);
    ASSERT(header.common.size == sizeof(PageHeader));
    // printf("  Page: vpn=0x%lx, blocks_valid=0x%lx\n", header.vpn,
    //       header.blocks_valid);

    Page *page = xu_new0(Page);
    page->vpn = header.vpn;
    for (int i = 0; i < N_PAGE_BLOCKS; i++) {
        bool valid_block = (header.blocks_valid >> i) & 1;
        page->bytes_valid[i] = valid_block ? BLOCK_VALID_FLAG : 0;
        if (valid_block) {
            fread(&page->data[i * BLOCK_SIZE], BLOCK_SIZE, 1, fp);
        }
    }

    return page;
}

static Region *
read_region(FILE *fp)
{
    RegionHeader header;

    fread(&header, sizeof(header), 1, fp);
    ASSERT(header.common.magic == MAGIC_REGION);
    ASSERT(header.common.size == sizeof(RegionHeader));

    Region *region = xu_new0_vl_n(Region, Page *, header.n_pages);
    region->vpn_start = header.vpn_start;
    region->vpn_end = header.vpn_end;
    region->n_pages = header.n_pages;
    // printf("Region: vpn_start=0x%lx vpn_end=0x%lx\n", header.vpn_start,
    //        header.vpn_end);

    for (int i = 0; i < header.n_pages; i++) {
        region->pages[i] = read_page(fp);
    }

    return region;
}

static RegionList *
read_regions(FILE *fp)
{
    RegionListHeader header;

    fread(&header, sizeof(header), 1, fp);
    ASSERT(header.common.magic == MAGIC_REGION_LIST);
    ASSERT(header.common.size == sizeof(RegionListHeader));

    RegionList *region_list = xu_new0_vl_n(RegionList, Region *,
                                           header.n_regions);
    region_list->n_regions = header.n_regions;
    region_list->flags = header.flags;

    for (int i = 0; i < header.n_regions; i++) {
        region_list->regions[i] = read_region(fp);
    }

    return region_list;
}

void
free_regions(RegionList *regions)
{
    if (regions == NULL)
        return;

    for (int i = 0; i < regions->n_regions; i++) {
        Region *region = regions->regions[i];

        for (int j = 0; j < region->n_pages; j++) {
            xu_free(region->pages[j]);
        }
        xu_free(region);
    }
    xu_free(regions);
}

static void
read_registers(FILE *fp, Registers *registers)
{
    RegistersHeader header;

    fread(&header, sizeof(header), 1, fp);
    ASSERT(header.common.magic == MAGIC_REGISTERS);
    ASSERT(header.common.size == sizeof(RegistersHeader));

    registers->pc = header.pc;
    registers->regs_valid = header.regs_valid;
    registers->csr_regs_valid = header.csr_regs_valid;

    for (int i = 0; i < N_RISCV_GPR + N_RISCV_FP_REGS; i++) {
        if ((header.regs_valid >> i) & 1) {
            fread(&registers->regs[i], sizeof(registers->regs[i]), 1, fp);
            // printf("x%d=0x%lx\n",i,registers->regs[i]);
        }
    }
    for (int i = 0; i < N_RISCV_CSR; i++) {
        if ((header.csr_regs_valid >> i) & 1) {
            fread(&registers->csr_regs[i], sizeof(registers->csr_regs[i]), 1,
                  fp);
            // printf("x%d=0x%lx\n",i,registers->csr_regs[i]);
        }
    }
}

void
read_snapshot_file(FILE *fp, RegionList **p_insn_regions,
                   RegionList **p_data_regions, Registers *registers)
{
    read_registers(fp, registers);
    ASSERT(fp != NULL);
    ASSERT(p_insn_regions != NULL && p_data_regions != NULL);
    ASSERT(registers != NULL);

    // printf("insn::::::::::::::::::::::::::::::::::::::\n");
    *p_insn_regions = read_regions(fp);
    uint32_t flags = (*p_insn_regions)->flags;
    if ((flags & REGIONS_FLAG_EXEC_MASK) != REGIONS_FLAG_INSN) {
        xu_error("Expected instruction regions first in the snapshot file.\n");
        xu_exit();
    }
    // printf("data::::::::::::::::::::::::::::::::::::::\n");
    *p_data_regions = read_regions(fp);
    flags = (*p_data_regions)->flags;
    if ((flags & REGIONS_FLAG_EXEC_MASK) != REGIONS_FLAG_DATA) {
        xu_error("Expected data regions second in the snapshot file.\n");
        xu_exit();
    }
}
