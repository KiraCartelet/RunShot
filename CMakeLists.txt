# RunShot: Live program state capture and playback tool
#
# Copyright (C) 2024 Microprocessor R&D Center (MPRC), Peking University
# All rights reserved.
#
# Author: Chun Yang

cmake_minimum_required(VERSION 3.10)

project(runshot)

include_directories(include)

add_subdirectory(qemu-plugins)
add_subdirectory(tools)
