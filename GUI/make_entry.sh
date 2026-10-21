#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <payload.elf | snapshot.img>" >&2
  exit 1
fi

IN="$(realpath "$1")"
[[ -f "$IN" ]] || { echo "[ERR] input not found: $IN" >&2; exit 1; }

READELF="${READELF:-riscv64-unknown-elf-readelf}"
GCC="${GCC:-riscv64-unknown-elf-gcc}"

command -v "$READELF" >/dev/null 2>&1 || READELF="readelf"
command -v "$READELF" >/dev/null 2>&1 || { echo "[ERR] readelf not found" >&2; exit 1; }
command -v "$GCC" >/dev/null 2>&1 || { echo "[ERR] riscv64-unknown-elf-gcc not found" >&2; exit 1; }

# defaults (overridable)
DEFAULT_TOHOST="${TOHOST_ADDR:-0x84999990}"
DEFAULT_FROMHOST="${FROMHOST_ADDR:-0x84999998}"

STUB_S="entry.S"
STUB_LD="entry.ld"
OUT_ELF="entry.riscv"
REG_IMAGE_WORDS=68   # pc + 64 regs + 3 csr_regs

MODE="$(python3 - "$IN" <<'PY'
import sys, struct
p = sys.argv[1]
b = open(p,'rb').read(8)
if len(b) >= 4 and b[:4] == b'\x7fELF':
    print("elf"); raise SystemExit
if len(b) >= 4:
    (magic,) = struct.unpack_from("<I", b, 0)
    if magic == 0x52454753:  # "REGS"
        print("snapshot"); raise SystemExit
print("unknown")
PY
)"

if [[ "$MODE" == "unknown" ]]; then
  echo "[ERR] cannot detect file type: $IN (not ELF and not REGS snapshot)" >&2
  exit 1
fi

echo "[INFO] input : $IN"
echo "[INFO] mode  : $MODE"

read -r -d '' COMMON_BOOT <<'EOF' || true
    /* enable FP (mstatus.FS=Dirty) to avoid illegal on fsd/fld */
    li   t0, (3 << 13)
    csrs mstatus, t0

    /* install trap handler */
    la   t0, trap_frame
    csrw mscratch, t0
    la   t0, trap_handler
    csrw mtvec, t0
EOF

# Trap handler: handles ECALL (mcause=8/9/11) but only exit/exit_group,
# plus misaligned load/store (mcause=4/6).
read -r -d '' TRAP_HANDLER <<'EOF' || true
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
EOF

# ------------------------------------------------------------------------------
# ELF mode
# ------------------------------------------------------------------------------
if [[ "$MODE" == "elf" ]]; then
  ENTRY_HEX="$("$READELF" -l "$IN" 2>/dev/null | awk '/Entry point/{print $3; exit}' || true)"
  [[ -n "${ENTRY_HEX:-}" ]] || { echo "[ERR] cannot parse Entry point from ELF: $IN" >&2; exit 1; }
  [[ "$ENTRY_HEX" == 0x* ]] || ENTRY_HEX="0x$ENTRY_HEX"

  TOHOST_HEX="$("$READELF" -s "$IN" 2>/dev/null | awk '$NF=="tohost"{print $2; exit}' || true)"
  FROMHOST_HEX="$("$READELF" -s "$IN" 2>/dev/null | awk '$NF=="fromhost"{print $2; exit}' || true)"

  if [[ -z "${TOHOST_HEX:-}" ]]; then
    TOHOST_HEX="${DEFAULT_TOHOST#0x}"
    echo "[WARN] tohost not found; default tohost=${DEFAULT_TOHOST}"
  fi
  if [[ -z "${FROMHOST_HEX:-}" ]]; then
    FROMHOST_HEX="${DEFAULT_FROMHOST#0x}"
    echo "[WARN] fromhost not found; default fromhost=${DEFAULT_FROMHOST}"
  fi

  cat > "$STUB_S" <<EOF
    .section .text
    .globl _start
    .option norvc
    .option norelax

_start:
$COMMON_BOOT
    li   t0, $ENTRY_HEX
    jr   t0
1:  j    1b

$TRAP_HANDLER

# BEGIN AUTO TOHOST/FROMHOST (do not edit inside)
    .globl tohost
    .set   tohost, 0x$TOHOST_HEX
    .globl fromhost
    .set   fromhost, 0x$FROMHOST_HEX
# END AUTO TOHOST/FROMHOST
EOF

# ------------------------------------------------------------------------------
# Snapshot mode: REGS at start only
# ------------------------------------------------------------------------------
else
  TOHOST_HEX="${DEFAULT_TOHOST#0x}"
  FROMHOST_HEX="${DEFAULT_FROMHOST#0x}"

  mapfile -t SNAP_LINES < <(python3 - "$IN" <<'PY'
import struct, sys

MAGIC_REGS = 0x52454753  # "REGS"
HDR_REGS_SZ = 25         # CommonHeader(8) + u64 pc + u64 regs_valid + u8 csr_regs_valid
N_GPRS = 32
N_FPRS = 32
N_CSRS = 3
N_REGS = N_GPRS + N_FPRS

p = sys.argv[1]
b = open(p, 'rb').read()
n = len(b)

if n < HDR_REGS_SZ:
    raise SystemExit("[ERR] snapshot too small")

# ---- parse REGS at file start ----
magic, size, pc, regs_valid, csr_regs_valid = struct.unpack_from("<IIQQB", b, 0)
if magic != MAGIC_REGS or size != HDR_REGS_SZ:
    raise SystemExit("[ERR] bad REGS header")

pos = HDR_REGS_SZ

regs = [0] * N_REGS
for i in range(N_REGS):
    if (regs_valid >> i) & 1:
        if pos + 8 > n:
            raise SystemExit(f"[ERR] truncated reg payload at idx={i}")
        regs[i] = struct.unpack_from("<Q", b, pos)[0]
        pos += 8

csr_regs = [0] * N_CSRS
for i in range(N_CSRS):
    if (csr_regs_valid >> i) & 1:
        if pos + 8 > n:
            raise SystemExit(f"[ERR] truncated csr payload at idx={i}")
        csr_regs[i] = struct.unpack_from("<Q", b, pos)[0]
        pos += 8

# low 32 bits still correspond to x0..x31
if ((regs_valid >> 2) & 1) == 0:
    raise SystemExit("[ERR] gp_regs_valid missing sp(bit2)")
if ((regs_valid >> 1) & 1) == 0:
    raise SystemExit("[ERR] gp_regs_valid missing ra(bit1)")

saved_ra = regs[1]

print(f"pc=0x{pc:016x}")
print(f"mask=0x{regs_valid:016x}")
print(f"csr_mask=0x{csr_regs_valid:02x}")
print(f"saved_ra=0x{saved_ra:016x}")
print(f"jump_ra=0x{saved_ra:016x}")

img = [0] * (1 + N_REGS + N_CSRS)
img[0] = pc
for i in range(N_REGS):
    img[i + 1] = regs[i]

for i in range(N_CSRS):
    img[1 + N_REGS + i] = csr_regs[i]

for v in img:
    print(f"0x{v:016x}")
PY
)

  SNAP_PC_LINE="${SNAP_LINES[0]}"
  SNAP_MASK_LINE="${SNAP_LINES[1]}"
  SNAP_CSR_MASK_LINE="${SNAP_LINES[2]}"
  SNAP_SAVED_RA_LINE="${SNAP_LINES[3]}"
  SNAP_JUMP_RA_LINE="${SNAP_LINES[4]}"

  REG_WORDS=("${SNAP_LINES[@]:5:${REG_IMAGE_WORDS}}")

  echo "[INFO] snapshot.${SNAP_PC_LINE}"
  echo "[INFO] snapshot.${SNAP_SAVED_RA_LINE}"
  echo "[INFO] snapshot.${SNAP_JUMP_RA_LINE}"

  cat > "$STUB_S" <<EOF
    .section .text
    .globl _start
    .option norvc
    .option norelax

_start:
$COMMON_BOOT
    /* base pointer must be in ra (your mature restore) */
    la   ra, snapshot_regs_image
    j    restore_regs

1:  j    1b

    .globl restore_regs
restore_regs:
/* restore FPRs: regs[32..63] == f0..f31 */
    fld f0,  264(ra)
    fld f1,  272(ra)
    fld f2,  280(ra)
    fld f3,  288(ra)
    fld f4,  296(ra)
    fld f5,  304(ra)
    fld f6,  312(ra)
    fld f7,  320(ra)
    fld f8,  328(ra)
    fld f9,  336(ra)
    fld f10, 344(ra)
    fld f11, 352(ra)
    fld f12, 360(ra)
    fld f13, 368(ra)
    fld f14, 376(ra)
    fld f15, 384(ra)
    fld f16, 392(ra)
    fld f17, 400(ra)
    fld f18, 408(ra)
    fld f19, 416(ra)
    fld f20, 424(ra)
    fld f21, 432(ra)
    fld f22, 440(ra)
    fld f23, 448(ra)
    fld f24, 456(ra)
    fld f25, 464(ra)
    fld f26, 472(ra)
    fld f27, 480(ra)
    fld f28, 488(ra)
    fld f29, 496(ra)
    fld f30, 504(ra)
    fld f31, 512(ra)

    ld a0,   520(ra)
    ld a1,   528(ra)
    ld a2,   536(ra)
    csrw fflags, a0
    csrw frm,    a1
    csrw fcsr,   a2

    ld s1,   80(ra)
    ld s2,  152(ra)
    ld s3,  160(ra)
    ld s4,  168(ra)
    ld s5,  176(ra)
    ld s6,  184(ra)
    ld s7,  192(ra)
    ld s8,  200(ra)
    ld s9,  208(ra)
    ld s10, 216(ra)
    ld s11, 224(ra)

    ld t0,  48(ra)
    ld t1,  56(ra)
    ld t2,  64(ra)
    ld t3, 232(ra)
    ld t4, 240(ra)
    ld t5, 248(ra)
    ld t6, 256(ra)

    ld sp,  24(ra)
    ld gp,  32(ra)
    ld tp,  40(ra)
    ld s0,  72(ra)

    ld a0,  88(ra)
    ld a1,  96(ra)
    ld a2, 104(ra)
    ld a3, 112(ra)
    ld a4, 120(ra)
    ld a5, 128(ra)
    ld a6, 136(ra)
    ld a7, 144(ra)

    ld ra,  16(ra)
    jr ra

$TRAP_HANDLER

    .section .rodata
    .balign 8
    /* $SNAP_PC_LINE */
    /* $SNAP_MASK_LINE */
    /* $SNAP_CSR_MASK_LINE */
    /* $SNAP_SAVED_RA_LINE */
    /* $SNAP_JUMP_RA_LINE */
    .globl snapshot_regs_image
snapshot_regs_image:
EOF

  for w in "${REG_WORDS[@]}"; do
    echo "    .dword $w" >> "$STUB_S"
  done

  cat >> "$STUB_S" <<EOF

# BEGIN AUTO TOHOST/FROMHOST (do not edit inside)
    .globl tohost
    .set   tohost, 0x$TOHOST_HEX
    .globl fromhost
    .set   fromhost, 0x$FROMHOST_HEX
# END AUTO TOHOST/FROMHOST
EOF
fi

if [[ ! -f "$STUB_LD" ]]; then
  cat > "$STUB_LD" <<'EOF'
ENTRY(_start)
SECTIONS
{
  . = 0x80000000;
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
}
EOF
  echo "[INFO] generated $STUB_LD"
fi

"$GCC" -nostdlib -nostartfiles -march=rv64imafdc_zicsr_zifencei -mabi=lp64d \
  -T "$STUB_LD" "$STUB_S" -o "$OUT_ELF"

echo "[OK] generated: $(realpath "$OUT_ELF")"
echo "[TIP] run:"
echo "  ./simulator-chipyard.harness-*Config +permissive +loadmem=$IN +permissive-off ./$OUT_ELF"
echo "[TIP] override tohost/fromhost defaults:"
echo "  TOHOST_ADDR=0x84999990 FROMHOST_ADDR=0x84999998 ./make_entry.sh $IN"
