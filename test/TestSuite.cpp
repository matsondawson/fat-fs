/**
 * Runs every test group. File system tests run once directly on the RAM disk and,
 * when the block cache is compiled in, again through tiny caches so that every
 * file system test also exercises cache eviction, write-back and read ahead.
 */
#include "FatTests.h"
#include "TestEnv.h"

static int runGroups(const char *pass, const char *filter) {
	int failed = 0;
	failed += TestFramework::run(FAT_TESTS, FAT_TEST_COUNT, filter, pass);
	failed += TestFramework::run(LONG_NAME_TESTS, LONG_NAME_TEST_COUNT, filter, pass);
	failed += TestFramework::run(ROBUSTNESS_TESTS, ROBUSTNESS_TEST_COUNT, filter, pass);
	return failed;
}

int runFatTests(const char *filter, uint32_t ramDiskSectors) {
	g_config.ramDiskSectors = ramDiskSectors;
	g_config.randomIterations = ramDiskSectors >= 1024 ? 2000 : 300;
	int failed = 0;

	printf("Configuration: long file names %s, block cache %s, FAT12/16 %s\n",
#ifdef MFS_LONG_FILE_NAMES
		"ON",
#else
		"OFF",
#endif
#ifdef MFS_BLOCK_CACHE
		"ON",
#else
		"OFF",
#endif
#ifdef MFS_FAT12_16
		"ON"
#else
		"OFF"
#endif
	);

	g_config.cacheSlots = 0;
	failed += runGroups("direct", filter);

#ifdef MFS_BLOCK_CACHE
	g_config.cacheSlots = 2;
	g_config.writeThrough = false;
	failed += runGroups("cache-2-slot-write-back", filter);

	g_config.cacheSlots = 1;
	g_config.writeThrough = true;
	failed += runGroups("cache-1-slot-write-through", filter);

	// Reading ahead into 3 slots: evicts more, and reads past what the driver asks for
	g_config.cacheSlots = 3;
	g_config.writeThrough = false;
	g_config.readAhead = true;
	failed += runGroups("cache-3-slot-read-ahead", filter);
	g_config.readAhead = false;

	g_config.cacheSlots = 0;
	g_config.writeThrough = false;
	failed += TestFramework::run(CACHE_TESTS, CACHE_TEST_COUNT, filter, "cache");
#endif

#ifdef MFS_FAT12_16
	// The same tests on FAT16 and FAT12 volumes, directly and through a write back cache
	g_config.format = FatFormat::FAT16;
	failed += runGroups("fat16", filter);
	failed += TestFramework::run(FAT12_16_TESTS, FAT12_16_TEST_COUNT, filter, "fat16");
	g_config.format = FatFormat::FAT12;
	failed += runGroups("fat12", filter);
	failed += TestFramework::run(FAT12_16_TESTS, FAT12_16_TEST_COUNT, filter, "fat12");
#ifdef MFS_BLOCK_CACHE
	g_config.cacheSlots = 2;
	failed += runGroups("fat12-cache-2-slot-write-back", filter);
	g_config.cacheSlots = 0;
#endif
	g_config.format = FatFormat::FAT32;
#endif

	printf("\n==== %s: %d failed test(s) ====\n", failed ? "FAILED" : "ALL PASSED", failed);
	return failed;
}
