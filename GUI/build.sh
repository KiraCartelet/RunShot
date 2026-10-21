#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage:"
  echo "  $0 <main.c> [other1.c other2.c ...] [-o output]"
  echo
  echo "Example:"
  echo "  $0 basicmath_small.c rad2deg.c cubic.c isqrt.c -o basicmath"
  exit 1
fi

CC="${CC:-riscv64-unknown-linux-gnu-gcc}"
WRAP_SRC="${WRAP_SRC:-wrap_stack.c}"

[[ -f "$WRAP_SRC" ]] || { echo "[ERR] wrap source not found: $WRAP_SRC" >&2; exit 1; }

OUT_NAME=""
SRC_LIST=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    -o)
      shift
      [[ $# -gt 0 ]] || { echo "[ERR] -o requires an argument" >&2; exit 1; }
      OUT_NAME="$1"
      ;;
    *)
      SRC_LIST+=("$1")
      ;;
  esac
  shift
done

[[ ${#SRC_LIST[@]} -ge 1 ]] || { echo "[ERR] no source files provided" >&2; exit 1; }

for f in "${SRC_LIST[@]}"; do
  [[ -f "$f" ]] || { echo "[ERR] source not found: $f" >&2; exit 1; }
done

if [[ -z "$OUT_NAME" ]]; then
  OUT_NAME="$(basename "${SRC_LIST[0]}" .c)"
fi

echo "[INFO] CC        = $CC"
echo "[INFO] WRAP_SRC  = $WRAP_SRC"
echo "[INFO] OUTPUT    = $OUT_NAME"
echo "[INFO] SOURCES   = ${SRC_LIST[*]} $WRAP_SRC"

"$CC" \
  -O2 -static -fno-pie -no-pie \
  -mcmodel=medany -mno-relax \
  -fno-builtin -mstrict-align \
  "${SRC_LIST[@]}" "$WRAP_SRC" \
  -Wl,-Ttext-segment=0x85000000 \
  -Wl,-z,max-page-size=4096 \
  -Wl,--no-relax \
  -Wl,--wrap=__libc_start_main \
  -o "$OUT_NAME.riscv" \
  -lm

echo "[OK] built: $OUT_NAME.riscv"
