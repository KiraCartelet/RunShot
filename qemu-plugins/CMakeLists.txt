# RunShot: Live program state capture and playback tool
#
# Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
# All rights reserved.
#
# Author: Chun Yang

# QEMU source path
set(QEMU_SRC_PATH "~/riscv-work/qemu-9.2.0" CACHE PATH "Path to the QEMU source code")
if(NOT QEMU_SRC_PATH)
    message(FATAL_ERROR "QEMU_SRC_PATH is not set. "
            "Please provide the path to the QEMU source code.")
endif()

# Required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

# C flags for qemu plugins
set(PLUGIN_CFLAGS ${GLIB_CFLAGS} -fPIC -Wall)
include_directories(${QEMU_SRC_PATH} ${QEMU_SRC_PATH}/include/qemu)

# Instruction count plugin
add_library(icount SHARED icount.c)
target_compile_options(icount PRIVATE ${PLUGIN_CFLAGS})

# Snapshot plugin
add_library(snapshot SHARED)
target_sources(snapshot PRIVATE
               image.c
               snapshot.c
               syscall-riscv64.c
               elfparse.c
)
target_compile_options(snapshot PRIVATE ${PLUGIN_CFLAGS})
