# RunShot: Live program state capture and playback tool

RunShot is a process snapshot toolkit based on QEMU plugins. It includes a
snapshot saving plugin and a recovery tool. RunShot can capture register state,
instruction pages and data pages in memory, and syscall logs while a program is
running. The generated snapshot can then be inspected, analyzed, and replayed
with the recovery tool.

[Project Document](./README.md) | [项目文档](./README_zh.md)


## Features

QEMU plugin (`snapshot`):

- Captures register state at an optional checkpoint, and starts recording the
  required in-memory instruction pages, data pages, and syscall logs.

- Replaces syscall instructions (`ecall`) and provides syscall handlers, making
  it possible to support Linux programs in a bare-metal environment.

- Saves the snapshot file `snapshot.img`, which can be restored in both QEMU and
  Chipyard.

---

Recovery tool (`runshot`):

- `info` subcommand: inspect the contents of a snapshot file.

- `play` subcommand: replay the contents of a snapshot file.

---

TODO:

- Program fragment extraction.

- Support for more syscalls.


## Project Structure

```
RunShot
├── CMakeLists.txt
├── README.md
├── /cmake-build-debug/
├── /include/
├── /qemu-plugin/
│  ├── icount.c
│  ├── snapshot.c
│  └── ...
└── /tools/
   ├── info.c
   ├── play.c
   ├── main.c
   └── ...
```

## Quick Start

### Requirements

1. RISC-V toolchain (RV64G)
2. User-mode QEMU for RISC-V (QEMU 9.2.0, built with plugin support)

### Installation

1.

### Build

```sh
mkdir build
cmake -B build -S . -DRV_TOOLCHAIN=/path/to/rv-toolchain
cd build
make
```

After building, the output directory should look like this:

```
/build/
├── /qemu-plugins/
│  ├── libicount.so
│  ├── libsnapshot.so
│  └── ...
├── /tools/
│  ├── runshot
│  └── ...
└── ...
```

### Usage Example

1. Currently, recovery supports programs cross-compiled with glibc
   (`riscv64-unknown-linux-gnu-gcc`).

  - If the program calls math library functions, you may need to explicitly
    link `libm` by adding `-lm` at the end of the command line.
  - ASLR (address space layout randomization) may need to be enabled to ensure
    stable execution.

```sh
riscv64-unknown-linux-gnu-gcc -O2 -static -fno-pie -no-pie -mcmodel=medany -mno-relax \
                        -fno-builtin -mstrict-align example.c wrap_stack.c \
                        -Wl,--wrap=__libc_start_main -Wl,-Ttext-segment=0x85000000 \
                        -Wl,-z,max-page-size=4096 -Wl,--no-relax -o example (-lm)
# or
./build.sh example.c -o example

# Enable ASLR
echo 2 | sudo tee /proc/sys/kernel/randomize_va_space
```

2. Run the program and save a snapshot (`k` means recording a checkpoint at the
   k-th selectable breakpoint; the default value is 1).

```sh
qemu-riscv64 -plugin ./libsnapshot.so,k=1,elf=./example -d plugin -D /dev/stdout ./example
```

When the program exits, a snapshot file containing registers, memory data pages,
and syscall logs will be generated:

```
snapshot.img
```

3. Inspect and replay the snapshot on the QEMU side.

```sh
qemu-riscv64 ./runshot info snapshot.img
qemu-riscv64 ./runshot play snapshot.img
```

4. Inspect and replay the snapshot on the Chipyard side.

```sh
# Generate the entry RISC-V program entry.riscv
./make_entry.sh snapshot.img
# chipyard/sims/verilator (use an absolute path for loadsnap)
./simulator-chipyard.harness-***Config +permissive +loadsnap=snapshot.img +permissive-off entry.riscv (+vcdfile=wave.fst)
```
