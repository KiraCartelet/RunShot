    /* trap_frame layout = xN * 8 bytes */
    .equ TF_SP,  (2*8)
    .equ TF_A0,  (10*8)
    .equ TF_A4,  (14*8)
    .equ TF_A5,  (15*8)
    .equ TF_A7,  (17*8)

    .align 2
trap_handler:
    /* swap: sp <- trap_frame, mscratch <- old sp */
    csrrw sp, mscratch, sp

    /* save regs to trap_frame (x0..x31 slots) */
    sd zero,   0(sp)
    sd ra,     8(sp)
    /* x2(original sp) is now in mscratch, fill later */
    sd gp,    24(sp)
    sd tp,    32(sp)
    sd t0,    40(sp)
    sd t1,    48(sp)
    sd t2,    56(sp)
    sd s0,    64(sp)
    sd s1,    72(sp)
    sd a0,    80(sp)
    sd a1,    88(sp)
    sd a2,    96(sp)
    sd a3,   104(sp)
    sd a4,   112(sp)
    sd a5,   120(sp)
    sd a6,   128(sp)
    sd a7,   136(sp)
    sd s2,   144(sp)
    sd s3,   152(sp)
    sd s4,   160(sp)
    sd s5,   168(sp)
    sd s6,   176(sp)
    sd s7,   184(sp)
    sd s8,   192(sp)
    sd s9,   200(sp)
    sd s10,  208(sp)
    sd s11,  216(sp)
    sd t3,   224(sp)
    sd t4,   232(sp)
    sd t5,   240(sp)
    sd t6,   248(sp)

    /* save original sp into x2 slot */
    csrr t0, mscratch
    sd   t0, TF_SP(sp)

    /* dispatch by mcause */
    csrr t0, mcause

    /* illegal instruction */
    li   t1, 2
    beq  t0, t1, handle_illegal_dbg

    /* ecall: 8/9/11 */
    li   t1, 8
    beq  t0, t1, handle_ecall
    li   t1, 9
    beq  t0, t1, handle_ecall
    li   t1, 11
    beq  t0, t1, handle_ecall

    /* misaligned load/store */
    li   t1, 4
    beq  t0, t1, handle_load_misaligned
    li   t1, 6
    beq  t0, t1, handle_store_misaligned

trap_hang:
    j trap_hang

/* ------------------ Illegal debug (mcause=2) ------------------ */
/* 记录：mepc/insn、orig_sp、*(u32*)(sp+16)、a4、a5，然后 hang */
handle_illegal_dbg:
    csrr t0, mepc
    lwu  t1, 0(t0)              /* 取 32-bit 指令（低16位就是压缩半字） */

    la   t2, dbg_mepc
    sd   t0, 0(t2)
    la   t2, dbg_insn
    sw   t1, 0(t2)

    /* orig_sp */
    ld   t3, TF_SP(sp)
    la   t2, dbg_sp
    sd   t3, 0(t2)

    /* safe read *(u32*)(orig_sp+16) using lbu */
    addi t4, t3, 16
    lbu  t0, 0(t4)
    lbu  t1, 1(t4)
    lbu  t5, 2(t4)
    lbu  t6, 3(t4)
    slli t1, t1, 8
    slli t5, t5, 16
    slli t6, t6, 24
    or   t0, t0, t1
    or   t0, t0, t5
    or   t0, t0, t6
    la   t2, dbg_sp16_u32
    sw   t0, 0(t2)

    /* a4/a5 at trap time */
    ld   t0, TF_A4(sp)
    la   t2, dbg_a4
    sd   t0, 0(t2)

    ld   t0, TF_A5(sp)
    la   t2, dbg_a5
    sd   t0, 0(t2)

1:  j 1b

/* ------------------ ECALL handler (mcause=8/9/11) ------------------ */
/* only handles exit / exit_group */
handle_ecall:
    /* ensure instruction at mepc is ecall: 0x00000073 */
    csrr t0, mepc
    lwu  t1, 0(t0)
    li   t2, 0x73
    bne  t1, t2, trap_hang

    /* sysno = a7 */
    ld   t3, TF_A7(sp)

    /* exit / exit_group never return */
    li   t4, 93                 /* __NR_exit */
    beq  t3, t4, handle_sys_exit
    li   t4, 94                 /* __NR_exit_group */
    beq  t3, t4, handle_sys_exit

    /* unsupported ecall */
    j trap_hang

/* exit / exit_group: write tohost and spin forever */
handle_sys_exit:
    /* exit code is a0 */
    ld   t0, TF_A0(sp)

    /* cmd = (code<<1) | 1 */
    slli t0, t0, 1
    ori  t0, t0, 1

    /* fromhost = 0 */
    la   t1, fromhost
    sd   zero, 0(t1)

    /* tohost = cmd */
    la   t1, tohost
    sd   t0, 0(t1)

2:  j 2b

/* ------------------ Misaligned load/store handler (mcause=4/6) ------------------ */
/* Supports: LW/LD and SW/SD (32-bit instructions only) */

handle_load_misaligned:
    csrr t0, mepc
    lwu  t1, 0(t0)              /* inst */

    /* opcode LOAD=0x03 */
    andi t2, t1, 0x7f
    li   t3, 0x03
    bne  t2, t3, trap_hang

    /* funct3 */
    srli t2, t1, 12
    andi t2, t2, 7              /* funct3 in t2 */

    /* rd, rs1, immI */
    srli t3, t1, 7
    andi t3, t3, 31             /* rd */
    srli t4, t1, 15
    andi t4, t4, 31             /* rs1 */
    srli t5, t1, 20             /* imm[11:0] */
    slli t5, t5, 52
    srai t5, t5, 52             /* signext imm */

    /* eff addr = rs1_val + imm */
    slli t6, t4, 3
    add  t6, sp, t6
    ld   a3, 0(t6)              /* a3 = rs1_val */
    add  a3, a3, t5             /* a3 = eff addr */

    /* LW (funct3=2) */
    li   t0, 2
    beq  t2, t0, emulate_lw
    /* LD (funct3=3) */
    li   t0, 3
    beq  t2, t0, emulate_ld
    j trap_hang

emulate_lw:
    lbu  t0, 0(a3)
    lbu  t1, 1(a3)
    lbu  t4, 2(a3)
    lbu  t5, 3(a3)
    slli t1, t1, 8
    slli t4, t4, 16
    slli t5, t5, 24
    or   t0, t0, t1
    or   t0, t0, t4
    or   t0, t0, t5
    slli t0, t0, 32
    srai t0, t0, 32             /* signext */
    j lwld_wb

emulate_ld:
    lbu  t0, 0(a3)
    lbu  t1, 1(a3)
    lbu  t4, 2(a3)
    lbu  t5, 3(a3)
    lbu  t6, 4(a3)
    lbu  a0, 5(a3)
    lbu  a1, 6(a3)
    lbu  a2, 7(a3)

    slli t1, t1, 8
    slli t4, t4, 16
    slli t5, t5, 24
    slli t6, t6, 32
    slli a0, a0, 40
    slli a1, a1, 48
    slli a2, a2, 56

    or   t0, t0, t1
    or   t0, t0, t4
    or   t0, t0, t5
    or   t0, t0, t6
    or   t0, t0, a0
    or   t0, t0, a1
    or   t0, t0, a2
    j lwld_wb

lwld_wb:
    beqz t3, lwld_skip
    slli t5, t3, 3
    add  t5, sp, t5
    sd   t0, 0(t5)
lwld_skip:
    csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    j restore_and_mret

handle_store_misaligned:
    csrr t0, mepc
    lwu  t1, 0(t0)              /* inst */

    /* opcode STORE=0x23 */
    andi t2, t1, 0x7f
    li   t3, 0x23
    bne  t2, t3, trap_hang

    /* funct3 */
    srli t2, t1, 12
    andi t2, t2, 7

    /* rs1, rs2, immS */
    srli t4, t1, 15
    andi t4, t4, 31             /* rs1 */
    srli t3, t1, 20
    andi t3, t3, 31             /* rs2 */

    srli t5, t1, 7
    andi t5, t5, 0x1f           /* imm4_0 */
    srli t6, t1, 25
    slli t6, t6, 5
    or   t5, t5, t6             /* imm11_0 */
    slli t5, t5, 52
    srai t5, t5, 52             /* signext */

    /* eff addr = rs1_val + imm */
    slli t6, t4, 3
    add  t6, sp, t6
    ld   a3, 0(t6)
    add  a3, a3, t5

    /* rs2_val */
    slli t5, t3, 3
    add  t5, sp, t5
    ld   t0, 0(t5)

    /* SW (funct3=2) */
    li   t1, 2
    beq  t2, t1, emulate_sw
    /* SD (funct3=3) */
    li   t1, 3
    beq  t2, t1, emulate_sd
    j trap_hang

emulate_sw:
    sb   t0, 0(a3)
    srli t0, t0, 8
    sb   t0, 1(a3)
    srli t0, t0, 8
    sb   t0, 2(a3)
    srli t0, t0, 8
    sb   t0, 3(a3)
    j swsd_done

emulate_sd:
    sb   t0, 0(a3)
    srli t0, t0, 8
    sb   t0, 1(a3)
    srli t0, t0, 8
    sb   t0, 2(a3)
    srli t0, t0, 8
    sb   t0, 3(a3)
    srli t0, t0, 8
    sb   t0, 4(a3)
    srli t0, t0, 8
    sb   t0, 5(a3)
    srli t0, t0, 8
    sb   t0, 6(a3)
    srli t0, t0, 8
    sb   t0, 7(a3)

swsd_done:
    csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    j restore_and_mret

restore_and_mret:
    ld ra,     8(sp)
    ld gp,    24(sp)
    ld tp,    32(sp)
    ld t0,    40(sp)
    ld t1,    48(sp)
    ld t2,    56(sp)
    ld s0,    64(sp)
    ld s1,    72(sp)
    ld a0,    80(sp)
    ld a1,    88(sp)
    ld a2,    96(sp)
    ld a3,   104(sp)
    ld a4,   112(sp)
    ld a5,   120(sp)
    ld a6,   128(sp)
    ld a7,   136(sp)
    ld s2,   144(sp)
    ld s3,   152(sp)
    ld s4,   160(sp)
    ld s5,   168(sp)
    ld s6,   176(sp)
    ld s7,   184(sp)
    ld s8,   192(sp)
    ld s9,   200(sp)
    ld s10,  208(sp)
    ld s11,  216(sp)
    ld t3,   224(sp)
    ld t4,   232(sp)
    ld t5,   240(sp)
    ld t6,   248(sp)

    /* swap back: sp <- old sp, mscratch <- trap_frame */
    csrrw sp, mscratch, sp
    mret

    .section .bss
    .balign 16
trap_frame:
    .space 256

    .balign 8
dbg_mepc: .dword 0
dbg_sp:   .dword 0
dbg_a4:   .dword 0
dbg_a5:   .dword 0
    .balign 4
dbg_insn:     .word 0
dbg_sp16_u32: .word 0
