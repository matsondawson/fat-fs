# Generic bare-metal ARM Cortex-M toolchain file for building the driver library
# for any microcontroller:
#
#   cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DFAT_ARM_CPU=cortex-m4
#
# Produces libfat.a to link into your firmware. The toolchain's bin directory must
# be on PATH, or set ARM_TOOLCHAIN_DIR.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(ARM_TOOLCHAIN_DIR "" CACHE PATH "Directory containing arm-none-eabi-gcc (empty = PATH)")
set(FAT_ARM_CPU "cortex-m0plus" CACHE STRING "Value for -mcpu (cortex-m0plus = RP2040, cortex-m33 = RP2350)")

if(ARM_TOOLCHAIN_DIR)
  set(_prefix "${ARM_TOOLCHAIN_DIR}/arm-none-eabi-")
else()
  set(_prefix "arm-none-eabi-")
endif()
set(CMAKE_C_COMPILER   ${_prefix}gcc)
set(CMAKE_CXX_COMPILER ${_prefix}g++)
set(CMAKE_ASM_COMPILER ${_prefix}gcc)
set(CMAKE_AR           ${_prefix}ar)

# There is no OS to link test programs against.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT   "-mcpu=${FAT_ARM_CPU} -mthumb -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=${FAT_ARM_CPU} -mthumb -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
