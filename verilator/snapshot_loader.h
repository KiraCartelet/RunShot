#pragma once
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// 这里要 include 你自己的快照结构定义
#include "load.h" // 如果有
#include "snapshot.h" // RegionList/Region/Page/Registers/read_snapshot_file/free_regions
#include "xutils.h" // 如果你还想用 xu_error/xu_exit/ASSERT（否则用 assert/printf）

// fesvr 的 memif_t
#include <fesvr/memif.h>

static uint64_t *used_regions_vpn_start = NULL;
static int used_regions_index = 0;
static int region_num = 0;

static bool
is_region_used(uint64_t vpn_start)
{
    assert(region_num > 0);
    for (int i = region_num - 1; i >= 0; i--) {
        if (vpn_start == used_regions_vpn_start[i])
            return true;
        else if (vpn_start > used_regions_vpn_start[i])
            return false;
    }
    return false;
}

static inline void
range_check_or_die(uint64_t addr, uint64_t len, uint64_t mem_base,
                   uint64_t mem_size)
{
    if (addr < mem_base || addr + len > mem_base + mem_size) {
        fprintf(stderr,
                "[snap] OOB write: addr=0x%lx len=0x%lx mem=[0x%lx,0x%lx)\n",
                (unsigned long)addr, (unsigned long)len,
                (unsigned long)mem_base, (unsigned long)(mem_base + mem_size));
        abort();
    }
}

static void
load_region_memif(Region *region, bool exec_flag, memif_t *memif,
                  uint64_t mem_base, uint64_t mem_size)
{
    bool region_used_flag = false;
    if (exec_flag) {
        used_regions_vpn_start[used_regions_index++] = region->vpn_start;
    } else {
        region_used_flag = is_region_used(region->vpn_start);
    }

    for (int i = 0; i < region->n_pages; i++) {
        Page *page = region->pages[i];
        if (page->vpn > 0xffffff)
            continue;
        uint64_t addr = ((uint64_t)(page->vpn)) << PAGE_SHIFT;

        // range_check_or_die(addr, PAGE_SIZE, mem_base, mem_size);

        // fprintf(stderr, "page vpn=0x%lx\n", page->vpn);
        if (!region_used_flag) {
            memif->write((addr_t)addr, PAGE_SIZE, page->data);
        } else {
            static uint8_t tmp[PAGE_SIZE]; // 也可以改成 malloc 避免 static
            memif->read((addr_t)addr, PAGE_SIZE, tmp);
            for (int j = 0; j < PAGE_SIZE; j++)
                tmp[j] |= page->data[j];
            memif->write((addr_t)addr, PAGE_SIZE, tmp);
        }
    }
}

// 对外入口：只装内存，不恢复寄存器
static int
load_snapshot_into_memif(const char *file_name, memif_t *memif,
                         uint64_t mem_base, uint64_t mem_size, int verbose)
{
    FILE *fp = fopen(file_name, "rb");
    if (!fp) {
        perror("open snapshot");
        return -1;
    }

    RegionList *insn_regions = NULL;
    RegionList *data_regions = NULL;
    Registers registers;
    read_snapshot_file(fp, &insn_regions, &data_regions, &registers);

    region_num = insn_regions->n_regions;
    used_regions_vpn_start = (uint64_t *)malloc(sizeof(uint64_t) * region_num);
    used_regions_index = 0;

    if (verbose)
        fprintf(stderr, "[snap] insn regions=%d data regions=%d\n",
                insn_regions->n_regions, data_regions->n_regions);

    for (int i = 0; i < insn_regions->n_regions; i++)
        load_region_memif(insn_regions->regions[i], true, memif, mem_base,
                          mem_size);

    for (int i = 0; i < data_regions->n_regions; i++)
        load_region_memif(data_regions->regions[i], false, memif, mem_base,
                          mem_size);

    free(used_regions_vpn_start);
    used_regions_vpn_start = NULL;

    free_regions(insn_regions);
    free_regions(data_regions);
    fclose(fp);

    // printf("1111111111111111111111111111111\n");
    //  registers 先别在 host 侧恢复；后续用 entry.riscv 或写到固定内存供 stub
    //  恢复
    return 0;
}
