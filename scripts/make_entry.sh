#!/usr/bin/env bash
set -euo pipefail

# Resolve helper sources relative to this script; write outputs in the caller's
# working directory.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

fail()
{
    echo "[ERR] $*" >&2
    exit 1
}

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 <payload.elf | snapshot.img>" >&2
    exit 1
fi

IN="$(realpath "$1")"
[[ -f "$IN" ]] || fail "input not found: $IN"

READELF="${READELF:-riscv64-unknown-elf-readelf}"
GCC="${GCC:-riscv64-unknown-elf-gcc}"

command -v "$READELF" >/dev/null 2>&1 || READELF="readelf"
command -v "$READELF" >/dev/null 2>&1 || fail "readelf not found"
command -v "$GCC" >/dev/null 2>&1 || fail "riscv64-unknown-elf-gcc not found"

# defaults (overridable)
DEFAULT_TOHOST="${TOHOST_ADDR:-0x84999990}"
DEFAULT_FROMHOST="${FROMHOST_ADDR:-0x84999998}"

STUB_S="entry.S"
STUB_LD="entry.ld"
OUT_ELF="entry.riscv"
REG_IMAGE_WORDS=68   # pc + 64 regs + 3 csr_regs
RESTORE_REGS="$(cat "$SCRIPT_DIR/entry/restore.S")"

MODE="$(python3 "$SCRIPT_DIR/snapshot_regs.py" --detect "$IN")"

if [[ "$MODE" == "unknown" ]]; then
    echo "[ERR] cannot detect file type: $IN (not ELF and not REGS snapshot)" >&2
    exit 1
fi

echo "[INFO] input : $IN"
echo "[INFO] mode  : $MODE"

COMMON_BOOT="$(cat "$SCRIPT_DIR/entry/boot.S")"

# Trap handler: handles ECALL (mcause=8/9/11) but only exit/exit_group,
# plus misaligned load/store (mcause=4/6).
TRAP_HANDLER="$(cat "$SCRIPT_DIR/entry/trap.S")"

# ------------------------------------------------------------------------------
# ELF mode
# ------------------------------------------------------------------------------
if [[ "$MODE" == "elf" ]]; then
    ENTRY_HEX="$("$READELF" -l "$IN" 2>/dev/null | awk '/Entry point/{print $3; exit}' || true)"
    [[ -n "${ENTRY_HEX:-}" ]] || fail "cannot parse Entry point from ELF: $IN"
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

    # Capture the parser status before mapfile: process substitution hides errors.
    SNAP_DATA="$(python3 "$SCRIPT_DIR/snapshot_regs.py" "$IN")"
    mapfile -t SNAP_LINES <<< "$SNAP_DATA"

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
    /* Keep the register-image base in ra until the final jump. */
    la   ra, snapshot_regs_image
    j    restore_regs

1:  j    1b

    .globl restore_regs
restore_regs:
$RESTORE_REGS

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
    cp "$SCRIPT_DIR/entry.ld" "$STUB_LD"
    echo "[INFO] generated $STUB_LD"
fi

"$GCC" -nostdlib -nostartfiles -march=rv64imafdc_zicsr_zifencei -mabi=lp64d \
    -T "$STUB_LD" "$STUB_S" -o "$OUT_ELF"

echo "[OK] generated: $(realpath "$OUT_ELF")"
echo "[TIP] run:"
echo "  ./simulator-chipyard.harness-*Config +permissive +loadmem=$IN +permissive-off ./$OUT_ELF"
echo "[TIP] override tohost/fromhost defaults:"
echo "  TOHOST_ADDR=0x84999990 FROMHOST_ADDR=0x84999998 ./make_entry.sh $IN"
