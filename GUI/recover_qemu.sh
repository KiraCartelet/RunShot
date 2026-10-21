#!/usr/bin/env bash
source ~/.bashrc

qemu-riscv64 -d plugin -D /dev/stdout /home/cartelet/Desktop/runshot/build/tools/runshot play snapshot.img
