cmake_minimum_required(VERSION 3.13)
# Configure (from repo root):
#   cmake -S firmware -B build-fw \
#     -DCMAKE_TOOLCHAIN_FILE=$PWD/firmware/cmake/arm-none-eabi.cmake
#   cmake --build build-fw
project(pocket_fmdsp_fw C ASM)

set(CMAKE_C_STANDARD 11)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# --- shared: the OPNA core + vendored PMD driver + player glue (cross-built) ---
set(CORE_SRC
  ${ROOT}/src/opna/opna.c
  ${ROOT}/src/opna/opna_fm.c
  ${ROOT}/src/opna/opna_ssg.c
  ${ROOT}/src/opna/opna_drum.c
  ${ROOT}/src/opna/opna_timer.c
  ${ROOT}/src/driver/player.c
)
set(VENDOR_SRC
  ${ROOT}/src/driver/vendor/fmdriver/fmdriver_pmd.c
  ${ROOT}/src/driver/vendor/fmdriver/fmdriver_common.c
  ${ROOT}/src/driver/vendor/fmdriver/ppz8.c
)
# The OPNA core + PMD driver are the CPU-bound hot path: build them -O3 for the
# real-time render (overrides the target-wide -Os). Vendored code also gets -w.
set_source_files_properties(${CORE_SRC}   PROPERTIES COMPILE_OPTIONS "-O3")
set_source_files_properties(${VENDOR_SRC} PROPERTIES COMPILE_OPTIONS "-w;-O3")

# ================= QEMU / semihosting simulator firmware =================
add_executable(pocketfm_sim
  startup/startup.c
  bsp/sim/semihosting.c
  bsp/sim/board_sim.c
  gui/gfx.c
  gui/ui.c
  app/main.c
  ${CORE_SRC}
  ${VENDOR_SRC}
)
target_include_directories(pocketfm_sim PRIVATE
  bsp bsp/sim bsp/primer2 gui app
  ${ROOT}/include ${ROOT}/src/opna ${ROOT}/src/driver/vendor
)
target_compile_options(pocketfm_sim PRIVATE -Os)
target_compile_definitions(pocketfm_sim PRIVATE PFM_SIM=1)
target_link_options(pocketfm_sim PRIVATE
  -T${CMAKE_CURRENT_SOURCE_DIR}/ld/qemu_netduino2.ld
  -Wl,-Map=${CMAKE_BINARY_DIR}/pocketfm_sim.map
)
set_target_properties(pocketfm_sim PROPERTIES
  LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ld/qemu_netduino2.ld)
add_custom_command(TARGET pocketfm_sim POST_BUILD
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:pocketfm_sim>)

# ================= Real STM32 Primer2 hardware targets =================
set(HW_LD ${CMAKE_CURRENT_SOURCE_DIR}/ld/stm32f103ve.ld)

function(add_hw_target name)
  add_executable(${name} ${ARGN})
  target_include_directories(${name} PRIVATE
    bsp bsp/primer2 gui
    ${ROOT}/include ${ROOT}/src/opna ${ROOT}/src/driver/vendor)
  target_compile_options(${name} PRIVATE -Os -Wall -Wextra)
  target_link_options(${name} PRIVATE -T${HW_LD} -Wl,-Map=${CMAKE_BINARY_DIR}/${name}.map)
  set_target_properties(${name} PROPERTIES LINK_DEPENDS ${HW_LD})
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${name}> ${CMAKE_BINARY_DIR}/${name}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${name}> ${CMAKE_BINARY_DIR}/${name}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${name}>)
endfunction()

# Stage 1: LED blinky (validates flash + execute)
add_hw_target(pocketfm_blinky startup/startup.c app/blinky.c)

# FreeRTOS kernel (minimal, fully static) — hardware target only
set(FREERTOS_SRC
  freertos/tasks.c freertos/list.c freertos/queue.c freertos/portable/port.c)
set_source_files_properties(${FREERTOS_SRC} PROPERTIES COMPILE_OPTIONS "-w;-O2")

# FatFs + SD-SPI (hardware target only)
set(FATFS_SRC fatfs/ff.c fatfs/ffunicode.c fatfs/diskio.c)
set_source_files_properties(${FATFS_SRC} PROPERTIES COMPILE_OPTIONS "-w;-Os")

# Stage 2: the player firmware (FSMC/ST7732 LCD + joystick + songs in flash)
add_hw_target(pocketfm_hw
  startup/startup.c
  bsp/primer2/board_primer2.c
  bsp/primer2/sd_spi.c
  gui/gfx.c
  gui/ui.c
  app/main_hw.c
  ${CORE_SRC}
  ${VENDOR_SRC}
  ${FREERTOS_SRC}
  ${FATFS_SRC}
)
target_include_directories(pocketfm_hw PRIVATE gen
  freertos freertos/include freertos/portable fatfs)
# place the per-sample render functions in 0-wait SRAM (.ramfunc)
target_compile_definitions(pocketfm_hw PRIVATE PFM_RAMFUNC=1 PFM_RTOS=1)
# emulate YM2203 (OPN, PC-9801-26K): 3 FM channels instead of the OPNA's 6
target_compile_definitions(pocketfm_hw PRIVATE PFM_FM_CH=3)
