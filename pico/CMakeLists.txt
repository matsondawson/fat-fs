# Raspberry Pi Pico firmware: runs the FAT driver test suite on the device
# (against RAM disks), then checks the driver on an SD card, and reports over
# USB serial.
#
# Build, flash and watch it with pico/pico.sh, which runs everything in Docker:
#
#   pico/pico.sh build                 # Pico / Pico W (RP2040), into pico/build/pico
#   pico/pico.sh --board pico2 build   # Pico 2 (RP2350), into pico/build/pico2
#   pico/pico.sh flash
#   pico/pico.sh monitor
#
# Or with a pico-sdk on the host:
#
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   cmake -S pico -B pico/build/pico -DPICO_BOARD=pico
#   cmake --build pico/build/pico
cmake_minimum_required(VERSION 3.13)

# pico (RP2040, also runs on a Pico W, leaving its wireless chip unused) or
# pico2 (RP2350). Each board needs its own build directory.
set(PICO_BOARD pico CACHE STRING "Board to build for: pico or pico2")

# Must come before project().
include(pico_sdk_import.cmake)

project(fat_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Driver features. The test firmware enables them all; turn any off with
# -DFAT_LONG_FILE_NAMES=OFF / -DFAT_BLOCK_CACHE=OFF / -DFAT_FAT12_16=OFF.
option(FAT_LONG_FILE_NAMES "Long file name support" ON)
option(FAT_BLOCK_CACHE "Block cache support" ON)
option(FAT_FAT12_16 "FAT12 and FAT16 support" ON)

# The SD card's wiring, passed to PicoSdCard at run time. The defaults are
# md_mpeg's: SPI1 on GP10 to GP13.
set(FAT_PICO_SD_SPI 1 CACHE STRING "SPI instance the SD card is on, 0 or 1")
set(FAT_PICO_SD_SCK 10 CACHE STRING "SD card SCK GPIO")
set(FAT_PICO_SD_MOSI 11 CACHE STRING "SD card MOSI GPIO")
set(FAT_PICO_SD_MISO 12 CACHE STRING "SD card MISO GPIO")
set(FAT_PICO_SD_CS 13 CACHE STRING "SD card CS GPIO")
set(FAT_PICO_SD_DETECT -1 CACHE STRING "SD card detect GPIO, low with a card in; -1 for none")
# 25 and 33 MHz read cleanly on jumper wires in md_mpeg; 50 MHz failed.
set(FAT_PICO_SD_HZ 25000000 CACHE STRING "SD card SPI clock in Hz")
# Off by default, so the card's contents are never changed unless asked for.
option(FAT_PICO_SD_WRITE_TEST "Also create, check and delete FATTEST.TMP on the SD card" OFF)

# The driver library, its portable test suite and the Pico SD card driver
# (fat::pico_sd) from the repository root.
add_subdirectory(.. fat)

add_executable(fat_pico_tests main.cpp SdCardTests.cpp)
target_link_libraries(fat_pico_tests PRIVATE fat::testsuite fat::pico_sd pico_stdlib)
target_compile_definitions(fat_pico_tests PRIVATE
  FAT_PICO_SD_SPI=${FAT_PICO_SD_SPI}
  FAT_PICO_SD_SCK=${FAT_PICO_SD_SCK}
  FAT_PICO_SD_MOSI=${FAT_PICO_SD_MOSI}
  FAT_PICO_SD_MISO=${FAT_PICO_SD_MISO}
  FAT_PICO_SD_CS=${FAT_PICO_SD_CS}
  FAT_PICO_SD_DETECT=${FAT_PICO_SD_DETECT}
  FAT_PICO_SD_HZ=${FAT_PICO_SD_HZ}
  FAT_PICO_SD_WRITE_TEST=$<BOOL:${FAT_PICO_SD_WRITE_TEST}>
)

pico_enable_stdio_usb(fat_pico_tests 1)
pico_enable_stdio_uart(fat_pico_tests 0)

pico_set_program_name(fat_pico_tests "fat_pico_tests")
pico_set_program_description(fat_pico_tests "FAT32 driver test harness")

# .uf2, .hex, .bin, .dis and .map outputs
pico_add_extra_outputs(fat_pico_tests)
