/*
 * RunShot: Live program state capture and playback tool
 *
 * Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 */

#ifndef __TOOLS_LOAD_H__
#define __TOOLS_LOAD_H__

#include <stdint.h>
#include <stdio.h>

#include "snapshot.h"

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

/* Free all regions in a RegionList. */
void free_regions(RegionList *regions);

/* Load all things from a snapshot file. */
void read_snapshot_file(FILE *fp, RegionList **p_insn_regions,
                        RegionList **p_data_regions, Registers *registers);

#endif /* __TOOLS_LOAD_H__ */
