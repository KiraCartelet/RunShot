"""Read the leading REGS record for the RISC-V entry stub.

The binary layout matches RegistersHeader in include/snapshot.h. The emitted
image contains PC, 32 GPRs, 32 FPRs, then fflags, frm and fcsr, all as u64 words.
"""

import argparse
import struct
import sys
from pathlib import Path

MAGIC_REGISTERS = 0x52454753
REGISTERS_HEADER = struct.Struct("<IIQQB")
WORD = struct.Struct("<Q")
N_REGISTERS = 64
N_CSRS = 3


def detect_format(path):
    """Inspect only the magic; full REGS validation happens during parsing."""
    with path.open("rb") as source:
        magic = source.read(4)
    if magic == b"\x7fELF":
        return "elf"
    if magic == struct.pack("<I", MAGIC_REGISTERS):
        return "snapshot"
    return "unknown"


def read_words(source, count, mask, label):
    """Expand the sparse payload, filling uncaptured registers with zero."""
    words = [0] * count
    for index in range(count):
        if mask & (1 << index):
            data = source.read(WORD.size)
            if len(data) != WORD.size:
                raise ValueError(f"truncated {label} payload at idx={index}")
            words[index] = WORD.unpack(data)[0]
    return words


def read_registers(path):
    with path.open("rb") as source:
        header = source.read(REGISTERS_HEADER.size)
        if len(header) != REGISTERS_HEADER.size:
            raise ValueError("snapshot too small")
        magic, size, pc, mask, csr_mask = REGISTERS_HEADER.unpack(header)
        if magic != MAGIC_REGISTERS or size != REGISTERS_HEADER.size:
            raise ValueError("bad REGS header")
        registers = read_words(source, N_REGISTERS, mask, "reg")
        csrs = read_words(source, N_CSRS, csr_mask, "csr")

    for index, name in ((2, "sp"), (1, "ra")):
        if not mask & (1 << index):
            raise ValueError(f"gp_regs_valid missing {name}(bit{index})")
    return pc, mask, csr_mask, registers, csrs


def emit_registers(path):
    pc, mask, csr_mask, registers, csrs = read_registers(path)
    print(f"pc=0x{pc:016x}")
    print(f"mask=0x{mask:016x}")
    print(f"csr_mask=0x{csr_mask:02x}")
    print(f"saved_ra=0x{registers[1]:016x}")
    # The existing restore sequence resumes at saved ra, not at captured pc.
    print(f"jump_ra=0x{registers[1]:016x}")
    for word in [pc, *registers, *csrs]:
        print(f"0x{word:016x}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--detect", action="store_true",
                        help="print elf, snapshot or unknown")
    parser.add_argument("input", type=Path)
    args = parser.parse_args()
    try:
        if args.detect:
            print(detect_format(args.input))
        else:
            emit_registers(args.input)
    except (OSError, ValueError) as error:
        print(f"[ERR] {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
