# RunShot: Live program state capture and playback tool
#
# Copyright (C) 2024-2026 Microprocessor R&D Center (MPRC), Peking University
# All rights reserved.
#
# Author: Chun Yang

# C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED TRUE)

include(rv-toolchain.cmake)

add_executable(${PROJECT_NAME})
target_sources(${PROJECT_NAME} PRIVATE
               info.c
               load.c
               main.c
               play.c
)
target_link_libraries(${PROJECT_NAME} PRIVATE
        -Wl,-Ttext-segment=10000000 -static
)
