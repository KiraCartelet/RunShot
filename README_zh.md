# RunShot: Live program state capture and playback tool

RunShot 是一个基于 QEMU 插件的进程快照工具集，包括快照保存插件与恢复工具。支持在程序运行过程中捕获寄存器现场，内存中的指令页、数据页与系统调用，并通过恢复工具对快照进行查看、分析与回放。

[Project Document](./README.md) | [项目文档](./README_zh.md)


## 功能特性

QEMU 插件 (snapshot):

- 在可选检查点处捕获寄存器状态，同时开始记录必要的内存指令页与数据页内容、系统调用日志

- 系统调用指令 ecall 替换，并提供系统调用处理程序，实现裸机环境下对Linux程序的支持

- 保存快照文件 snapshot.img，支持在 QEMU 与 Chipyard 中恢复

---

恢复工具 (runshot):

- info 子命令：查看快照文件内容

- play 子命令：重放快照内容

---

TODO:

- 程序片段截取

- 对更多系统调用的支持


## 项目结构说明

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

## 快速上手

### 配置要求

1. RISC-V toolchain (RV64G)
2. user-mode QEMU for RISC-V (QEMU 9.2.0，开启插件支持)

### 安装

1.

### 编译

```sh
mkdir build
cmake -B build -S . -DRV_TOOLCHAIN=/path/to/rv-toolchain
cd build
make
```

编译后，可得到文件目录如下:
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

### 使用示例

1. 目前支持使用 glibc（riscv64-unknown-linux-gnu-gcc）交叉编译后的程序进行恢复
  - 若程序调用了数学库函数，有时需要显式链接 libm，在命令行末尾添加 -lm
  - 可能需要开启ASLR（地址随机化）才能保证运行稳定
```sh
riscv64-unknown-linux-gnu-gcc -O2 -static -fno-pie -no-pie -mcmodel=medany -mno-relax \
                        -fno-builtin -mstrict-align example.c wrap_stack.c \
                        -Wl,--wrap=__libc_start_main -Wl,-Ttext-segment=0x85000000 \
                        -Wl,-z,max-page-size=4096 -Wl,--no-relax -o example (-lm)
# or
./build.sh example.c -o example

# 开启ASLR
echo 2 | sudo tee /proc/sys/kernel/randomize_va_space
```

2. 运行程序并保存快照(k为在第k个可选择断点处记录检查点，默认为1)

```sh
qemu-riscv64 -plugin ./libsnapshot.so,k=1,elf=./example -d plugin -D /dev/stdout ./example
```

运行结束时会生成包括寄存器、内存数据页与系统调用日志的快照文件：
```
snapshot.img
```

3. 查看与回放快照(QEMU端)

```sh
qemu-riscv64 ./runshot info snapshot.img
qemu-riscv64 ./runshot play snapshot.img
```

4. 查看与回放快照(Chipyard端)
```sh
# 生成入口 RISCV 程序 entry.riscv
./make_entry.sh snapshot.img
# chipyard/sims/verilator (loadsnap中使用绝对地址)
./simulator-chipyard.harness-***Config +permissive +loadsnap=snapshot.img +permissive-off entry.riscv (+vcdfile=wave.fst)
```
