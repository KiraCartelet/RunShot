#!/usr/bin/env bash
source ~/.bashrc

qemu-riscv64 -plugin /home/cartelet/Desktop/runshot/build/qemu-plugins/libsnapshot.so,k=1,mode=a,elf=/home/cartelet/Desktop/test.riscv /home/cartelet/Desktop/test.riscv
