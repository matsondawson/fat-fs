#ifndef net_mdawson_test_TestEnv
#define net_mdawson_test_TestEnv

/**
 * Shared helpers for the test suites: RAM disks (optionally behind the block
 * cache, depending on the pass being run), formatted volumes and data patterns.
 */

#include "TestFramework.h"

#include "net/mdawson/filesystem/CachedSectorDevice.h"
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/FatFormat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/Error.h"
#include "net/mdawson/filesystem/MemorySectorDevice.h"
#include "net/mdawson/filesystem/Partition.h"

using namespace Mfs;

/** Smallest convenient FAT32 volume with 512 byte clusters (68882 clusters). */
static const sector_t VOLUME_SECTORS = 70000;
static const uint32_t CLUSTER_BYTES = 512;
/** Sectors before the first FAT for volumes made by formatFat32. */
static const sector_t RESERVED_SECTORS = 32;

struct TestConfig {
	/** Max non-zero sectors per RAM disk. */
	uint32_t ramDiskSectors;
	/** 0 = file system tests use the RAM disk directly; else cache slots in between. */
	uint32_t cacheSlots;
	bool writeThrough;
	/** Cache reads the next sector ahead. */
	bool readAhead;
	/** Iterations for randomized tests. */
	uint32_t randomIterations;
	/** Format of the volumes Volume makes. */
	FatFormat format;
};

/** Clusters a new volume's root directory takes: 1 on FAT32, none for the fixed FAT12/16 root. */
uint32_t rootDirectoryClusters();

extern TestConfig g_config;

/** A RAM disk, seen through the block cache when the current pass uses one. */
struct Disk {
	MemorySectorDevice ram;
#ifdef MFS_BLOCK_CACHE
	CachedSectorDevice *cache;
#endif

	explicit Disk(sector_t sectors, uint32_t maxStored = 0);
	~Disk();
	Disk(const Disk&) = delete;
	Disk& operator=(const Disk&) = delete;

	/** The device the file system should use. */
	SectorDevice &device();
};

/**
 * A freshly formatted, mounted volume of g_config.format. FAT12/16 volumes are sized
 * for sectorsPerCluster (3000 or 20000 clusters), ignoring sectors.
 */
struct Volume {
	Disk disk;
	/** Null if formatting failed. */
	Fat *fat;
	/** Result of formatting and mounting (or of the last remount()). */
	md_error_t error;

	explicit Volume(uint8_t sectorsPerCluster = 1, sector_t sectors = VOLUME_SECTORS);
	~Volume();
	Volume(const Volume&) = delete;
	Volume& operator=(const Volume&) = delete;

	/** Unmount and mount again (flushing first). */
	MFS_NODISCARD md_error_t remount();
};

uint8_t pattern(uint32_t i, uint32_t seed);
void fillPattern(uint8_t *buffer, uint32_t length, uint32_t start, uint32_t seed);
bool matchesPattern(const uint8_t *buffer, uint32_t length, uint32_t start, uint32_t seed);

/** Write length pattern bytes to a new (or replaced) file in chunks. */
MFS_NODISCARD md_error_t writeFile(Fat &fat, const char *path, const char *name, uint32_t length, uint32_t seed, uint32_t chunk);

/** verifyFile's result when the file reads without error but its length or data is wrong. */
constexpr md_error_t TEST_DATA_MISMATCH = 0x7E570001;

/** Read a whole file in chunks and check it matches the pattern: 0, a driver error or TEST_DATA_MISMATCH. */
MFS_NODISCARD md_error_t verifyFile(Fat &fat, const char *path, const char *name, uint32_t length, uint32_t seed, uint32_t chunk);

/** Number of entries a DirectoryListing returns. */
MFS_NODISCARD md_error_t countEntries(Fat &fat, const char *path, int &count);
/** The same, or -1 on any error. */
int countEntries(Fat &fat, const char *path);

/** Number of used 32 byte slots (including long name entries) in a directory. */
MFS_NODISCARD md_error_t countUsedSlots(Fat &fat, const char *path, int &count);
/** The same, or -1 on any error. */
int countUsedSlots(Fat &fat, const char *path);

/** Deterministic pseudo random numbers (xorshift32). */
struct Random {
	uint32_t state;
	explicit Random(uint32_t seed) : state(seed ? seed : 1) {}
	uint32_t next() {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return state;
	}
	uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

/** Test groups. */
extern const TestFramework::TestCase FAT_TESTS[];
extern const int FAT_TEST_COUNT;
extern const TestFramework::TestCase LONG_NAME_TESTS[];
extern const int LONG_NAME_TEST_COUNT;
extern const TestFramework::TestCase ROBUSTNESS_TESTS[];
extern const int ROBUSTNESS_TEST_COUNT;
extern const TestFramework::TestCase CACHE_TESTS[];
extern const int CACHE_TEST_COUNT;
extern const TestFramework::TestCase FAT12_16_TESTS[];
extern const int FAT12_16_TEST_COUNT;

#endif
