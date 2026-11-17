#ifndef net_mdawson_test_FatTests
#define net_mdawson_test_FatTests

#include <stdint.h>

/**
 * Run the FAT driver test suite against RAM disks.
 *
 * @param filter Only run tests whose name contains this (null/empty = all).
 * @param ramDiskSectors Maximum number of non-zero sectors each RAM disk may hold
 *        (each costs 512 bytes plus 16 bytes of index). 256 is plenty.
 * @return number of failed tests.
 */
int runFatTests(const char *filter, uint32_t ramDiskSectors);

#endif
