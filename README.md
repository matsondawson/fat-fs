# fat

A small FAT32 filesystem driver for microcontrollers, written in C++ with no OS
dependencies. It talks to storage through a one-sector-at-a-time `SectorDevice`
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
  FatFormat                     formatFat32()
  MemorySectorDevice            sparse RAM disk
platform/posix/FileSectorDevice disk image files and block devices on Linux
platform/pico/PicoSdCard        SD card over SPI on the RP2040 and RP2350
tools/fatimg.cpp                command line tool for disk images
test/                           portable test suite + Linux image cross-checks
pico/                           Raspberry Pi Pico test firmware (pico.sh, Docker)
cmake/arm-none-eabi.cmake       toolchain file for other Cortex-M targets
```

## Options

| CMake option          | Define                | Default | What it does |
|-----------------------|-----------------------|---------|--------------|
| `FAT_LONG_FILE_NAMES` | `MFS_LONG_FILE_NAMES` | OFF (ON in `pico/`) | Long file names, UTF-8 API |
| `FAT_BLOCK_CACHE`     | `MFS_BLOCK_CACHE`     | ON      | `CachedSectorDevice` |

Code size on a Cortex-M0+ (`-Os`): about 7.7 KB with neither option, 8.6 KB with the
cache, 9.6 KB with long names and 10.5 KB with both. If you don't use CMake,
define the macros yourself.

## Building

### Linux (driver, test harness, tools)

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The test build covers every option combination (`plain`, `lfn`, `cache`,
`lfn_cache`), whatever the options above are set to:

- `unit_<variant>` (`build/fat_tests_<variant> [filter]`): the portable test suite
  on RAM disks, built with AddressSanitizer and UBSan (turn off with
  `-DFAT_SANITIZE=OFF`). The file system tests run three times: directly on
  the RAM disk, through a 2-slot write-back cache and through a 1-slot
  write-through cache, so every test also exercises cache eviction. There are
  also cache unit tests, a power loss test, corrupt volume tests and a
  randomized test that checks the driver against an in-memory model.
- `image_<variant>` (`test/image_tests.sh`): cross-checks the driver against
  `mkfs.fat`, `mtools` and `fsck.fat` using disk image files, including long
  names in both directions. It only runs when dosfstools and mtools are
  installed (`pacman -S dosfstools mtools`).

GitHub Actions (`.github/workflows/ci.yml`) runs all of this with gcc and clang,
builds the Pico and Pico 2 firmware, and builds the library for Cortex-M0+ and
M4 in every option combination. The workflow runs on every pull request. Making
the `CI result` check required in the branch protection rules blocks merging
until it passes.

Nothing touches a real drive. To try the driver by hand, use an image:

```sh
build/fatimg disk.img create 64          # 64MB image, formatted FAT32
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
`fat_pico_sd` target:

```cpp
#include "PicoSdCard.h"

PicoSdCard sd(spi1, 10, 11, 12, 13);             // SCK, MOSI, MISO, CS; 25 MHz
sd.setCardDetect(17);                            // optional, low with a card in
if (sd.begin()) { printf("%s\n", sd.lastError()); }
Partition partition(sd, Partition::primary(0));
Fat fat(partition);
```

`begin()` checks that the pins belong to that SPI instance. The card must be
the only device on its SPI bus.

### Other microcontrollers

Build just the library with the generic toolchain file and link `libfat.a`
into your firmware:

```sh
cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DFAT_ARM_CPU=cortex-m4
cmake --build build-arm
```

Or `add_subdirectory()` this repository from your firmware's CMake project and
link the `fat` target, as `pico/CMakeLists.txt` does.

## Using it

```cpp
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/GlobalError.h"
#include "net/mdawson/filesystem/Partition.h"

MySdCard sd;                                     // your SectorDevice
Partition partition(sd, Partition::primary(0));  // or Partition::WHOLE_DEVICE
Fat fat(partition);
if (mdError) { /* not FAT32, or no card */ }

Mfs::File log(fat, "/LOGS", "DATA.CSV", true);   // true = create if missing
log.seek(log.length());                          // append
log.write((const uint8_t*)"1,2,3\n", 6);
if (mdError) { /* handle */ }
```

With a block cache (`MFS_BLOCK_CACHE`) in front of the card:

```cpp
CachedSectorDevice cache(sd);                        // 4 slots = 2 KB by default
Partition partition(cache, Partition::primary(0));
Fat fat(partition);
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
- `File::close()` and the `File` destructor: flush, then close.
- `Fat::flush()`, and destroying the `Fat` object (unmount).
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

### Long file names

With `MFS_LONG_FILE_NAMES`, names and paths are UTF-8 and can be long
(`"/My Documents/Quarterly Report.txt"`). Matching ignores case (ASCII only).
A long name is created whenever a name isn't a plain upper case 8.3 name,
together with a unique `NAME~1.EXT` alias, and the file can be opened by either
name. `DirectoryListing::name()` returns the long name. Without the option,
long names are rejected with `File_invalidName`. Files that other systems
created with long names can still be opened by their 8.3 alias, and deleting
them also removes their long name entries.

Errors are reported through the global `mdError` (codes in `GlobalError.h`)
rather than exceptions, so the code works with `-fno-exceptions`.

To support new storage, implement `SectorDevice`: `read`, `write` and
`getBytesPerSector` (must be 512).

## Limitations

- FAT32 only. Mounting a FAT12/FAT16 volume fails with an error.
- Long names compare case-insensitively for ASCII letters only. Windows also
  ignores case for other alphabets.
- Two `File` objects on the same file don't share state: one won't see a size
  change made by the other until that one flushes.
- The FSInfo free-cluster count is marked "unknown" on the first change rather
  than kept up to date, as the spec allows. fsck.fat mentions this but doesn't
  treat it as an error.
- Timestamps default to 1980-01-01. Set `Mfs::getFatTime` to supply real time.
- Not thread safe; `mdError` is a single global.
