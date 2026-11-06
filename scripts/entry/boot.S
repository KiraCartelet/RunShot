    /* enable FP (mstatus.FS=Dirty) to avoid illegal on fsd/fld */
    li   t0, (3 << 13)
    csrs mstatus, t0

    /* install trap handler */
    la   t0, trap_frame
    csrw mscratch, t0
    la   t0, trap_handler
    csrw mtvec, t0
