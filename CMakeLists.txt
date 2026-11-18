cmake_minimum_required(VERSION 3.13)

project(fat VERSION 0.1.0 LANGUAGES C CXX)

# Building this directory directly (rather than via add_subdirectory from a
# firmware project such as pico/) enables the host test harness and tools.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(FAT_IS_TOP_LEVEL ON)
else()
  set(FAT_IS_TOP_LEVEL OFF)
endif()

if(FAT_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
  set(FAT_HOST_DEFAULT ON)
else()
  set(FAT_HOST_DEFAULT OFF)
endif()

option(FAT_BUILD_TESTS "Build the host test harness" ${FAT_HOST_DEFAULT})
option(FAT_BUILD_TOOLS "Build the POSIX image tool (fatimg)" ${FAT_HOST_DEFAULT})

if(FAT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(FAT_LONG_FILE_NAMES "Long file name (VFAT) support (defines MFS_LONG_FILE_NAMES)" OFF)
option(FAT_BLOCK_CACHE "Include the CachedSectorDevice block cache (defines MFS_BLOCK_CACHE)" ON)
option(FAT_FAT12_16 "FAT12 and FAT16 volumes as well as FAT32 (defines MFS_FAT12_16)" OFF)
option(FAT_INSTALL "Generate install rules for the library and its CMake package" ${FAT_IS_TOP_LEVEL})

set(FAT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/mdawson/filesystem/CachedSectorDevice.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/mdawson/filesystem/Chain.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/mdawson/filesystem/Fat.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/mdawson/filesystem/FatFormat.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/mdawson/filesystem/File.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/mdawson/filesystem/LongName.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/mdawson/filesystem/MemorySectorDevice.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/mdawson/filesystem/Partition.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/net/mdawson/filesystem/Sector.cpp
)

# fat_library(<target> <long file names ON/OFF> <block cache ON/OFF> <FAT12/16 ON/OFF>)
function(fat_library target lfn cache fat1216)
  add_library(${target} STATIC ${FAT_SOURCES})
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_compile_features(${target} PUBLIC cxx_std_11)
  if(lfn)
    target_compile_definitions(${target} PUBLIC MFS_LONG_FILE_NAMES)
  endif()
  if(cache)
    target_compile_definitions(${target} PUBLIC MFS_BLOCK_CACHE)
  endif()
  if(fat1216)
    target_compile_definitions(${target} PUBLIC MFS_FAT12_16)
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

# fat_testsuite(<target> <library>): the portable test suite (also used on the Pico).
function(fat_testsuite target library)
  add_library(${target} STATIC EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/test/CacheTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/CountingSectorDevice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/Fat1216Tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/FatTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/LongNameTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/RobustnessTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/TestEnv.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/TestFramework.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/TestSuite.cpp
  )
  target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/test)
  target_link_libraries(${target} PUBLIC ${library})
endfunction()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# The driver: portable, no OS dependencies. Link fat::fat into firmware or host
# code and supply a SectorDevice for your storage. The real target names start
# with mfs_ so they don't clash with targets in the project that includes this.
# ---------------------------------------------------------------------------
fat_library(mfs_fat ${FAT_LONG_FILE_NAMES} ${FAT_BLOCK_CACHE} ${FAT_FAT12_16})
set_target_properties(mfs_fat PROPERTIES OUTPUT_NAME fat EXPORT_NAME fat)
add_library(fat::fat ALIAS mfs_fat)

if(NOT FAT_IS_TOP_LEVEL)
  fat_testsuite(mfs_fat_testsuite mfs_fat)
  add_library(fat::testsuite ALIAS mfs_fat_testsuite)
endif()

# ---------------------------------------------------------------------------
# Raspberry Pi Pico (RP2040/RP2350): SD card over SPI, on pins chosen at run
# time. Only defined when this directory is added from a pico-sdk project.
# ---------------------------------------------------------------------------
if(PICO_SDK_VERSION_STRING)
  add_library(mfs_fat_pico_sd STATIC ${CMAKE_CURRENT_SOURCE_DIR}/platform/pico/PicoSdCard.cpp)
  target_include_directories(mfs_fat_pico_sd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform/pico)
  target_link_libraries(mfs_fat_pico_sd PUBLIC mfs_fat hardware_spi hardware_gpio hardware_dma pico_time)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mfs_fat_pico_sd PRIVATE -Wall -Wextra)
  endif()
  add_library(fat::pico_sd ALIAS mfs_fat_pico_sd)
endif()

# ---------------------------------------------------------------------------
# Host only: POSIX device backend (image files, /dev/sdX) and tools.
# ---------------------------------------------------------------------------
if(FAT_BUILD_TOOLS OR FAT_BUILD_TESTS)
  add_library(mfs_fat_posix STATIC platform/posix/FileSectorDevice.cpp)
  target_include_directories(mfs_fat_posix PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform/posix)
  target_link_libraries(mfs_fat_posix PUBLIC mfs_fat)
  add_library(fat::posix ALIAS mfs_fat_posix)

  add_executable(fatimg tools/fatimg.cpp)
  target_link_libraries(fatimg PRIVATE mfs_fat_posix)
endif()

# ---------------------------------------------------------------------------
# Install: the library, its headers and a CMake package, so that another
# project can use find_package(fat CONFIG) and link fat::fat. The package
# carries the MFS_* defines the library was built with.
# ---------------------------------------------------------------------------
if(FAT_INSTALL)
  include(CMakePackageConfigHelpers)

  install(TARGETS mfs_fat EXPORT fatTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")
  install(EXPORT fatTargets
    NAMESPACE fat::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fat)

  configure_package_config_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/fatConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/fatConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fat)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/fatConfigVersion.cmake
    COMPATIBILITY SameMinorVersion)
  install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/fatConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/fatConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fat)
endif()

if(FAT_BUILD_TESTS)
  enable_testing()

  find_program(MKFS_FAT mkfs.fat)
  find_program(FSCK_FAT fsck.fat)
  find_program(MTOOLS_MCOPY mcopy)
  find_program(BASH bash)
  if(MKFS_FAT AND FSCK_FAT AND MTOOLS_MCOPY AND BASH)
    set(FAT_IMAGE_TESTS ON)
  else()
    set(FAT_IMAGE_TESTS OFF)
    message(STATUS "dosfstools/mtools not found: image cross-check tests disabled")
  endif()

  option(FAT_SANITIZE "Build host tests with address/undefined sanitizers" ON)
  find_package(Threads REQUIRED)

  # Every feature combination is built and tested, whatever the options above say.
  foreach(variant plain lfn cache lfn_cache fat1216 lfn_cache_fat1216)
    set(lfn OFF)
    set(cache OFF)
    set(fat1216 OFF)
    if(variant MATCHES "lfn")
      set(lfn ON)
    endif()
    if(variant MATCHES "cache")
      set(cache ON)
    endif()
    if(variant MATCHES "fat1216")
      set(fat1216 ON)
    endif()

    fat_library(fat_${variant} ${lfn} ${cache} ${fat1216})
    fat_testsuite(fat_testsuite_${variant} fat_${variant})
    add_executable(fat_tests_${variant} test/host_main.cpp)
    target_link_libraries(fat_tests_${variant} PRIVATE fat_testsuite_${variant} Threads::Threads)
    add_test(NAME unit_${variant} COMMAND fat_tests_${variant})

    add_library(fat_posix_${variant} STATIC platform/posix/FileSectorDevice.cpp)
    target_include_directories(fat_posix_${variant} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform/posix)
    target_link_libraries(fat_posix_${variant} PUBLIC fat_${variant})
    add_executable(fatimg_${variant} tools/fatimg.cpp)
    target_link_libraries(fatimg_${variant} PRIVATE fat_posix_${variant})

    set(sanitized fat_${variant} fat_testsuite_${variant} fat_tests_${variant} fat_posix_${variant} fatimg_${variant})
    if(FAT_SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      foreach(target ${sanitized})
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
      endforeach()
    endif()

    # Cross-check against mkfs.fat / mtools / fsck.fat on disk image files.
    if(FAT_IMAGE_TESTS)
      add_test(NAME image_${variant}
        COMMAND ${BASH} ${CMAKE_CURRENT_SOURCE_DIR}/test/image_tests.sh
                $<TARGET_FILE:fatimg_${variant}> ${CMAKE_CURRENT_BINARY_DIR}/image-tests-${variant})
    endif()
  endforeach()
endif()
