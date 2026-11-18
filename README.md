# fat

A small FAT32 filesystem driver for microcontrollers, written in C++ with no OS
dependencies, with optional FAT12 and FAT16 support. It talks to storage through a one-sector-at-a-time `SectorDevice`
interface, so it runs on anything you can read and write 512-byte sectors on:
an SD card, SPI flash, a RAM disk, or a disk image file on Linux.

## Layout

```
src/net/mdawson/filesystem/   the driver (portable)
  SectorDevice.h                interface to implement for your storage
  Partition                     MBR partition support
  Fat                           volume (boot sector, FAT table)
  Chain                         cluster chain cursor
  File                          File, DirectoryListing, makeDirectory, toShortName
  LongName                      long file name (VFAT) helpers
  CachedSectorDevice            optional write-back block cache
  FatFormat                     formatFat32(), formatFat()
  MemorySectorDevice            sparse RAM disk
platform/posix/FileSectorDevice disk image files and block devices on Linux
platform/pico/PicoSdCard        SD card over SPI on the RP2040 and RP2350
tools/fatimg.cpp                command line tool for disk images
test/                           portable test suite + Linux image cross-checks
test/consumer/                  a project using the library via find_package or source
pico/                           Raspberry Pi Pico test firmware (pico.sh, Docker)
cmake/arm-none-eabi.cmake       toolchain file for other Cortex-M targets
cmake/fatConfig.cmake.in        CMake package for find_package(fat)
```

## Options

| CMake option          | Define                | Default | What it does |
|-----------------------|-----------------------|---------|--------------|
| `FAT_LONG_FILE_NAMES` | `MFS_LONG_FILE_NAMES` | OFF (ON in `pico/`) | Long file names, UTF-8 API |
| `FAT_BLOCK_CACHE`     | `MFS_BLOCK_CACHE`     | ON      | `CachedSectorDevice` |
| `FAT_FAT12_16`        | `MFS_FAT12_16`        | OFF (ON in `pico/`) | FAT12 and FAT16 volumes, and formatting them |
| `FAT_INSTALL`         |                       | ON when built on its own | Install rules and the CMake package |

Code size on a Cortex-M0+ (`-Os`), in KB:

| Long names | Cache | FAT32 only | With FAT12/16 |
|------------|-------|------------|---------------|
| off        | off   | 7.7        | 8.7           |
| off        | on    | 8.7        | 9.8           |
| on         | off   | 9.6        | 10.6          |
| on         | on    | 10.6       | 11.7          |

If you don't use CMake, define the macros yourself, the same for the library
and for your code. A mismatch fails to link (with undefined references to
`Mfs::abi_...` names) rather than running with the wrong class layouts.

## Using it from another project

Everything is in namespace `Mfs`. The CMake target is `fat::fat`; it carries
the include directory and the `MFS_*` defines it was built with, so your code
always sees the same options as the library.

From source, with `FetchContent` (or `add_subdirectory()` on a copy). The
tests and tools are only built when this repository is the top level project:

```cmake
include(FetchContent)
FetchContent_Declare(fat
  GIT_REPOSITORY https://github.com/matsondawson/fat-fs.git
  GIT_TAG        v0.1.0)
set(FAT_LONG_FILE_NAMES ON CACHE BOOL "" FORCE)   # options before MakeAvailable
FetchContent_MakeAvailable(fat)

target_link_libraries(myapp PRIVATE fat::fat)
```

Or install it and use `find_package`:

```sh
cmake -S . -B build-lib -DFAT_BUILD_TESTS=OFF -DFAT_BUILD_TOOLS=OFF -DFAT_LONG_FILE_NAMES=ON
cmake --build build-lib
cmake --install build-lib --prefix /opt/fat
```

```cmake
find_package(fat 0.1 CONFIG REQUIRED)       # with -DCMAKE_PREFIX_PATH=/opt/fat
target_link_libraries(myapp PRIVATE fat::fat)
```

This installs `libfat.a`, the headers under `include/net/mdawson/` and
`lib/cmake/fat/`. Versions with the same major and minor number are compatible.
`test/consumer/` is a small project built both ways by `scripts/local-tests.sh`.

Other targets: `fat::pico_sd` (pico-sdk projects, see below) and `fat::posix`
(`FileSectorDevice`, host builds with tests or tools on).

## Building

### Linux (driver, test harness, tools)

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The test build covers the option combinations `plain`, `lfn`, `cache`,
`lfn_cache`, `fat1216` and `lfn_cache_fat1216`, whatever the options above are
set to:

- `unit_<variant>` (`build/fat_tests_<variant> [filter]`): the portable test suite
  on RAM disks, built with AddressSanitizer and UBSan (turn off with
  `-DFAT_SANITIZE=OFF`). The file system tests run three times: directly on
  the RAM disk, through a 2-slot write-back cache and through a 1-slot
  write-through cache, so every test also exercises cache eviction. There are
  also cache unit tests, a power loss test, corrupt volume tests and a
  randomized test that checks the driver against an in-memory model. With
  FAT12/16 support the tests run again on FAT16 and FAT12 volumes, along with
  tests of the fixed root directory, FAT12 entries split across sectors and
  the formatter.
- `image_<variant>` (`test/image_tests.sh`): cross-checks the driver against
  `mkfs.fat`, `mtools` and `fsck.fat` using disk image files, including long
  names in both directions. It only runs when dosfstools and mtools are
  installed (`pacman -S dosfstools mtools`).

### Before merging into main

There is no CI service; the tests run on your machine. Pull requests merge into
`main` only after `scripts/local-tests.sh` has passed on their latest commit:

```sh
git push                       # the PR branch
scripts/local-tests.sh         # a few minutes; reports "local-tests" to GitHub
```

The script runs the host tests with gcc and clang (warnings as errors, image
cross-checks included), builds the library for Cortex-M0+ and M4 in every option
combination, and builds the Pico and Pico 2 firmware. It needs a clean working
tree, and reports the result as the `local-tests` status of the commit. Branch
protection on `main` requires that status, for administrators too, and requires
the branch to be up to date with `main`. So after a new push, or when `main` moves
on, run it again. `--no-report` runs the tests without reporting.

Nothing touches a real drive. To try the driver by hand, use an image:

```sh
build/fatimg disk.img create 64          # 64MB image, formatted FAT32
build/fatimg_fat1216 -t 16 small.img create 16   # FAT16 (-t 12 for FAT12)
build/fatimg disk.img mkdir /LOGS
build/fatimg disk.img put notes.txt /LOGS/NOTES.TXT
build/fatimg disk.img ls /LOGS
build/fatimg disk.img cat /LOGS/NOTES.TXT
mdir -i disk.img ::LOGS                   # mtools sees the same files
```

`fatimg` also works on a USB flash drive (`build/fatimg /dev/sdX info`). Unmount
it first, and remember that `format`/`put`/`rm` write to the device. Add
`-p 0` when the drive has a partition table, as most do.

### Raspberry Pi Pico

`pico/pico.sh` builds, flashes and watches the test firmware with the
pico-sdk, Arm toolchain and picotool all in Docker, so nothing needs installing
on the host:

```sh
pico/pico.sh build                    # Pico / Pico W (RP2040), into pico/build/pico
pico/pico.sh --board pico2 build      # Pico 2 (RP2350), into pico/build/pico2
pico/pico.sh flash                    # reboots a running board into BOOTSEL itself
pico/pico.sh monitor                  # USB serial output for 60 s
```

The firmware runs the test suite on the chip against RAM disks, then checks the
driver on an SD card, every 10 seconds. It prints results over USB serial. The
LED (not on a Pico W) blinks slowly when all tests pass and fast when any fail.

The card check is read only: it reads the boot sector, mounts the FAT32 volume,
lists the root directory and reads up to 1 MB of its first file twice. A guard
refuses any write, so the card's contents can't change. To also create, check
and delete `FATTEST.TMP`, build with `-DFAT_PICO_SD_WRITE_TEST=ON`.

The card is on SPI1 by default: SCK GP10, MOSI GP11, MISO GP12, CS GP13. Change
the wiring with `-D` options, which CMake remembers in the build directory:

```sh
pico/pico.sh -DFAT_PICO_SD_SPI=0 -DFAT_PICO_SD_SCK=18 -DFAT_PICO_SD_MOSI=19 \
  -DFAT_PICO_SD_MISO=20 -DFAT_PICO_SD_CS=21 -DFAT_PICO_SD_DETECT=17 build
```

With a pico-sdk on the host instead, `cmake -S pico -B pico/build/pico` and
`cmake --build pico/build/pico` build the same firmware.

#### SD card driver for your firmware

`platform/pico/PicoSdCard` is a `SectorDevice` for an SD card in SPI mode on the
RP2040 or RP2350. The SPI instance and pins are constructor arguments, not
build settings, so the library is built once for any wiring. Adding this
repository with `add_subdirectory()` from a pico-sdk project defines the
`fat::pico_sd` target:

```cpp
#include "PicoSdCard.h"

Mfs::PicoSdCard sd(spi1, 10, 11, 12, 13);        // SCK, MOSI, MISO, CS; 25 MHz
sd.setCardDetect(17);                            // optional, low with a card in
if (sd.begin()) { printf("%s\n", sd.lastError()); }
Mfs::Partition partition(sd);
Mfs::Fat fat(partition);
if (partition.open(Mfs::Partition::primary(0)) || fat.mount()) { /* no card, or not FAT */ }
```

`begin()` checks that the pins belong to that SPI instance. The card must be
the only device on its SPI bus.

Reads use one multiple block read (CMD18) that stays open, so a sector read
after the one before it is just the next block from the card, without a
command or the card looking it up. Reading another sector, writing, or
`flush()` closes it. After each sector, DMA reads the next block into the
driver's buffer while your code works, so reading it is usually just a copy.
On an RP2040 with a 7.5 GB card at 20.8 MHz, reading 512 KB in order with
250 us of other work per sector ran at 1720 KB/s with read ahead and 972 KB/s
without; reading a file through the driver went from 796 KB/s with single
block reads to 1562 KB/s.

Read ahead uses two DMA channels. `begin()` claims them with
`dma_claim_unused_channel()`, so they can't clash with other code that claims
its channels the same way (the pico-sdk's own libraries do), and the destructor
releases them. If none are free it reads without read ahead
(`isReadAheadOn()` says which). To choose the channels yourself, claim them
and pass them to `setDmaChannels(rx, tx)` before `begin()`. The driver polls
for the DMA to finish instead of using DMA interrupts, so it leaves
`DMA_IRQ_0`/`DMA_IRQ_1` and their handlers to the rest of the program, and
the DMA only ever writes into the driver's own buffer. Code that uses fixed
channel numbers without claiming them can still collide with it: claim them
(`dma_channel_claim()`) at startup, before `begin()`. `setReadAhead(false)`
turns read ahead off.

### Other microcontrollers

Build just the library with the generic toolchain file and link `libfat.a`
into your firmware:

```sh
cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DFAT_ARM_CPU=cortex-m4
cmake --build build-arm
```

Or `add_subdirectory()` this repository from your firmware's CMake project and
link `fat::fat`, as `pico/CMakeLists.txt` does (see
[Using it from another project](#using-it-from-another-project)).

## Using it

```cpp
#include "net/mdawson/filesystem/Error.h"
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/Partition.h"

MySdCard sd;                                     // your Mfs::SectorDevice
Mfs::Partition partition(sd);
Mfs::Fat fat(partition);
if (Mfs::md_error_t error = partition.open(Mfs::Partition::primary(0))) { /* or Partition::WHOLE_DEVICE */ }
if (Mfs::md_error_t error = fat.mount()) { /* not FAT, or no card */ }

Mfs::File log(fat);
if (Mfs::md_error_t error = log.open("/LOGS", "DATA.CSV", true)) { /* true = create if missing */ }
log.seek(log.length());                          // append
const char line[] = "1,2,3\n";
if (Mfs::md_error_t error = log.write(reinterpret_cast<const uint8_t*>(line), sizeof(line) - 1)) { /* handle */ }

uint8_t buffer[64];
uint32_t bytesRead;
log.seek(0);
log.read(buffer, sizeof(buffer), bytesRead);     // fewer than asked at the end of the file

Mfs::DirectoryListing dir(fat);
if (dir.open("/LOGS") == 0) {
	while (dir.next()) {
		char name[256];
		dir.name(name, sizeof(name));
	}
	if (dir.error()) { /* the listing stopped on an error, not at the end */ }
}
```

With a block cache (`MFS_BLOCK_CACHE`) in front of the card:

```cpp
Mfs::CachedSectorDevice cache(sd);                   // 4 slots = 2 KB by default
Mfs::Partition partition(cache);
Mfs::Fat fat(partition);
```

`CachedSectorDevice cache(sd, 4, false, true)` also reads ahead: reading sector
N from the device reads N + 1 into the cache too, and reading a sector that was
read ahead reads the next, so a file read in order stays one sector ahead. A
failed read ahead (past the end of the card, say) is ignored. It only pays when
the device reads consecutive sectors faster than separate ones. On its own it
reads the same sectors earlier: `PicoSdCard`, which reads one sector per
command, read a file at 761 KB/s through it against 796 KB/s without.

### When data reaches the card

A write-back cache holds dirty sectors in RAM until they are evicted or
flushed. `File::write()` also only updates the file size in memory, except
for the first write to an empty file, which stores the entry immediately so
a new cluster chain is never left unreferenced. Data is written to the card by:

- `File::flush()`: stores the file's directory entry and flushes the device.
- `File::close()` and the `File` destructor: flush, then close. The destructor
  can't report an error, so call `close()` to find out whether it worked.
- Destroying the `Fat` object (unmount), which closes any `File`s still open
  on it. `Fat::flush()` flushes the device but not the sizes of open files.
- Destroying the `CachedSectorDevice`.

There is no timer. The driver has no clock and is not thread safe, so it must
never be called from an interrupt. For a periodic flush, call `flush()` from
your main loop, for example on the Pico:

```cpp
absolute_time_t nextFlush = make_timeout_time_ms(2000);
for (;;) {
	logFile.write(sample, sizeof(sample));
	if (time_reached(nextFlush)) {
		logFile.flush();                             // at most 2s of data at risk
		nextFlush = make_timeout_time_ms(2000);
	}
}
```

If power is lost, the volume still mounts. Everything written before the last
flush is intact; later writes may be partly there (`test_power_loss` checks
this). After a card swap, call `cache.invalidate()` to drop stale sectors.

### Memory and speed

Mounting a volume allocates two 512-byte buffers: one for the FAT, one for
everything else. Reads and writes of whole, sector-aligned 512 bytes go
straight between your buffer and the card; only partial sectors use the
volume's buffer. Nothing else is allocated while reading or writing a file.

The FAT buffer keeps the last FAT sector, and each open file remembers how
many of its next clusters follow one another, so a file whose clusters are
consecutive (usual on a freshly written card) looks up the FAT once per FAT
sector its chain covers (128 clusters on FAT32) rather than at every cluster,
even with several files read in turn.

### Long file names

With `MFS_LONG_FILE_NAMES`, names and paths are UTF-8 and can be long
(`"/My Documents/Quarterly Report.txt"`). Matching ignores case (ASCII only).
A long name is created whenever a name isn't a plain upper case 8.3 name,
together with a unique `NAME~1.EXT` alias, and the file can be opened by either
name. `DirectoryListing::name()` returns the long name. Without the option,
long names are rejected with `File_invalidName`. Files that other systems
created with long names can still be opened by their 8.3 alias, and deleting
them also removes their long name entries.

### FAT12 and FAT16

With `FAT_FAT12_16`, `Fat` also mounts FAT12 and FAT16 volumes, such as SD cards
of 2 GB and under as they come, and `getFormat()` says which it is. The API is
the same for all three. To format one:

```cpp
Mfs::formatFat(sd, sectorCount, Mfs::FatFormat::FAT16);   // or FAT12, FAT32
```

`formatFat` picks the smallest cluster size that keeps the cluster count within
the type's limits (FAT12 under 4085, FAT16 4085 to 65524), or fails with
`Fat_formatVolumeTooSmall` / `Fat_formatVolumeTooLarge` for a size you give.

### Errors and new storage

Every operation that can fail returns an `md_error_t`: 0 for success, or a code
from `Error.h` such as `Mfs::File_fileNotFound`. Other results come back through
reference parameters (`File::read(data, size, bytesRead)`). There are no
exceptions, so the code works with `-fno-exceptions`, and no global error
state. Objects are made in two steps, because a constructor can't return an
error: `Fat` then `mount()`, `Partition` then `open()`, `File` and
`DirectoryListing` then `open()`. `DirectoryListing::next()` and `find()`
return whether they found an entry, so they can drive a loop; when they return
false, `error()` tells the end of the directory (0) from a failure.

Functions that return an `md_error_t` are marked `[[nodiscard]]` (C++17, or
Clang's equivalent before that), so ignoring an error is a compiler warning.
Where an error really doesn't matter, cast the call to `void`.

To support new storage, implement `Mfs::SectorDevice`: `read`, `write` and
`getBytesPerSector` (must be 512).

### Threads

The driver has no locks. A volume, and the `File` and `DirectoryListing`
objects on it, must only be used from one thread at a time. It has no global
state, so threads that each use their own volume (two SD cards, say) don't
affect each other; the host tests check this.

## Limitations

- FAT12 and FAT16 need `FAT_FAT12_16`. Without it, mounting one fails with
  `Fat_Fat12FormatUnsupported` or `Fat_Fat16FormatUnsupported`.
- The FAT12/FAT16 root directory has a fixed number of entries (512 when the
  driver formats it). Adding one more fails with `Fat_rootDirectoryFull`;
  subdirectories grow as usual. A long name takes several entries.
- Long names compare case-insensitively for ASCII letters only. Windows also
  ignores case for other alphabets.
- A file can be open through only one `File` at a time; opening it again, by
  any of its names, fails with `File_alreadyOpen` until the first is closed.
  The check is per `Fat`, so don't mount the same volume twice.
- The FSInfo free-cluster count is marked "unknown" on the first change rather
  than kept up to date, as the spec allows. fsck.fat mentions this but doesn't
  treat it as an error.
- Timestamps default to 1980-01-01. Set `Mfs::getFatTime` to supply real time.
- Not thread safe: see [Threads](#threads).
