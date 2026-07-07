cmake_minimum_required(VERSION 3.13)
project(pocket_fmdsp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wshadow)

# --- pfmcore: freestanding OPNA emulator + PMD driver (target-portable) ---
add_library(pfmcore STATIC
  src/opna/opna.c
  src/opna/opna_fm.c
  src/opna/opna_ssg.c
  src/opna/opna_drum.c
  src/opna/opna_timer.c
  src/driver/pmd_load.c
)
target_include_directories(pfmcore PUBLIC include)
target_include_directories(pfmcore PRIVATE src/opna)

# --- vendored PMD driver (98fmplayer, BSD-2; drives our OPNA via callbacks) ---
add_library(pfmvendor STATIC
  src/driver/vendor/fmdriver/fmdriver_pmd.c
  src/driver/vendor/fmdriver/fmdriver_common.c
  src/driver/vendor/fmdriver/ppz8.c
)
target_include_directories(pfmvendor PUBLIC src/driver/vendor)
target_compile_options(pfmvendor PRIVATE -w) # vendored code: don't apply our strict warnings

# --- pfmdriver: player glue binding the driver to the OPNA core ---
add_library(pfmdriver STATIC src/driver/player.c)
target_include_directories(pfmdriver PUBLIC include)
target_include_directories(pfmdriver PRIVATE src/driver/vendor)
target_link_libraries(pfmdriver PUBLIC pfmcore pfmvendor)

# --- pfm: host simulator / CLI ---
add_executable(pfm
  host/pfm_host.c
  host/pfm_info.c
  host/wav.c
)
target_include_directories(pfm PRIVATE include host)
target_link_libraries(pfm pfmdriver pfmcore)
find_library(M_LIB m)
if(M_LIB)
  target_link_libraries(pfm ${M_LIB})
endif()
