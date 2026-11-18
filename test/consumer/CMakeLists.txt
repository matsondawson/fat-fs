# A project that uses the library the way another project would, either from an
# install (find_package) or from source (add_subdirectory, as FetchContent does).
# scripts/local-tests.sh builds and runs it both ways.
#
#   cmake -S test/consumer -B <dir> -DCMAKE_PREFIX_PATH=<install prefix>
#   cmake -S test/consumer -B <dir> -DFAT_SOURCE_DIR=<this repository>
cmake_minimum_required(VERSION 3.13)
project(fat_consumer CXX)

# A target named like the library's own, which must not clash with it
add_library(fat INTERFACE)

if(FAT_SOURCE_DIR)
  add_subdirectory(${FAT_SOURCE_DIR} fat-build)
else()
  find_package(fat 0.1 CONFIG REQUIRED)
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE fat::fat)
