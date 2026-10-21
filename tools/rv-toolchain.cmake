# RunShot: Live program state capture and playback tool
#
# Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
# All rights reserved.
#
# Author: Chun Yang

if(DEFINED RV_TOOLCHAIN)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR riscv64)
    set(CMAKE_C_COMPILER ${RV_TOOLCHAIN}/bin/riscv64-unknown-linux-gnu-gcc)
    set(CMAKE_CXX_COMPILER ${RV_TOOLCHAIN}/bin/riscv64-unknown-linux-gnu-g++)
    set(CMAKE_FIND_ROOT_PATH ${RV_TOOLCHAIN})
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
endif()
