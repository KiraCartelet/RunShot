#!/usr/bin/env bash
source ~/.bashrc

qemu-riscv64 -plugin /home/cartelet/Desktop/runshot/build/qemu-plugins/libsnapshot.so,k=1000,elf=/home/cartelet/Desktop/test.riscv \
             -d plugin -D /dev/stdout /home/cartelet/Desktop/test.riscv
