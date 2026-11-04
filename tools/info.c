/*
 * RunShot: Live program state capture and playback tool
 *
 * Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
 * All rights reserved.
 *
 * Author: Chun Yang
 */

#include <stdio.h>

#include "load.h"
#include "snapshot.h"
#include "xutils.h"

void
cmd_info(char *file_name, int verbose)
{
    FILE *fp = NULL;
    fp = fopen(file_name, "rb");
    if (fp == NULL) {
        xu_perror("Failed to open snapshot file '%s'", file_name);
        xu_exit();
    }

    RegionList *insn_regions = NULL;
    RegionList *data_regions = NULL;
    Registers registers;
    read_snapshot_file(fp, &insn_regions, &data_regions, &registers);

    free_regions(insn_regions);
    insn_regions = NULL;
    free_regions(data_regions);
    data_regions = NULL;

    fclose(fp);
    fp = NULL;
}
