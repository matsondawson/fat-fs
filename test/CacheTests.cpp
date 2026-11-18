/**
 * Tests for CachedSectorDevice on its own, plus I/O reduction and power loss
 * behaviour of the file system on top of it.
 */
#include "TestEnv.h"

#ifdef MFS_BLOCK_CACHE

#include "CountingSectorDevice.h"

using namespace Mfs;

static const sector_t SMALL_DISK = 1000;

static void sectorWith(uint8_t *buffer, uint8_t value) {
	memset(buffer, value, 512);
}

static bool ramHas(MemorySectorDevice &ram, sector_t sector, uint8_t value) {
	uint8_t buffer[512];
	if (ram.read(buffer, sector)) return false;
	for (int i = 0; i < 512; i++) {
		if (buffer[i] != value) return false;
	}
	return true;
}

static void test_read_miss_then_hit() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 4);
	uint8_t buffer[512];
	sectorWith(buffer, 7);
	CHECK_EQ(ram.write(buffer, 5), 0);

	memset(buffer, 0, sizeof(buffer));
	CHECK_EQ(cache.read(buffer, 5), 0);
	CHECK_EQ(buffer[100], 7);
	CHECK_EQ(cache.read(buffer, 5), 0);
	CHECK_EQ(cache.read(buffer, 5), 0);

	CHECK_EQ(counter.reads, 1);
	CHECK_EQ(cache.getStats().misses, 1);
	CHECK_EQ(cache.getStats().hits, 2);
	CHECK_EQ(cache.getStats().deviceReads, 1);
	cache.resetStats();
	CHECK_EQ(cache.getStats().hits, 0);
}

static void test_lru_eviction_order() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 2);
	uint8_t buffer[512];
	CHECK_EQ(cache.read(buffer, 1), 0);
	CHECK_EQ(cache.read(buffer, 2), 0);
	CHECK_EQ(cache.read(buffer, 1), 0); // 1 is now most recently used
	CHECK_EQ(cache.read(buffer, 3), 0); // evicts 2
	CHECK(cache.isCached(1));
	CHECK(!cache.isCached(2));
	CHECK(cache.isCached(3));
	CHECK_EQ(cache.read(buffer, 2), 0); // evicts 1
	CHECK(!cache.isCached(1));
	CHECK_EQ(counter.reads, 4);
}

static void test_write_back_only_on_evict_or_flush() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 2);
	uint8_t buffer[512];
	sectorWith(buffer, 0xAA);
	CHECK_EQ(cache.write(buffer, 10), 0);
	CHECK_EQ(cache.write(buffer, 10), 0); // rewriting a cached sector costs nothing
	CHECK_EQ(counter.writes, 0);
	CHECK_EQ(counter.reads, 0);  // a whole sector write needs no read
	CHECK_EQ(cache.getDirtyCount(), 1);
	CHECK(!ramHas(ram, 10, 0xAA));

	// Reading back comes from the cache.
	memset(buffer, 0, sizeof(buffer));
	CHECK_EQ(cache.read(buffer, 10), 0);
	CHECK_EQ(buffer[0], 0xAA);

	// Two more sectors force sector 10 out.
	CHECK_EQ(cache.read(buffer, 11), 0);
	CHECK_EQ(cache.read(buffer, 12), 0);
	CHECK_EQ(counter.writes, 1);
	CHECK(ramHas(ram, 10, 0xAA));
	CHECK_EQ(cache.getDirtyCount(), 0);

	// flush() writes remaining dirty sectors and flushes the device.
	sectorWith(buffer, 0xBB);
	CHECK_EQ(cache.write(buffer, 20), 0);
	CHECK_EQ(cache.flush(), 0);
	CHECK(ramHas(ram, 20, 0xBB));
	CHECK_EQ(counter.writes, 2);
	CHECK_EQ(counter.flushes, 1);
	CHECK_EQ(cache.getDirtyCount(), 0);
	CHECK_EQ(cache.flush(), 0); // nothing left to write
	CHECK_EQ(counter.writes, 2);
}

static void test_flush_writes_in_sector_order() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 4);
	uint8_t buffer[512];
	sectorWith(buffer, 1);
	static const sector_t order[] = { 40, 7, 300, 12 };
	for (int i = 0; i < 4; i++) CHECK_EQ(cache.write(buffer, order[i]), 0);
	CHECK_EQ(cache.flush(), 0);
	CHECK_EQ(counter.writeLogLength, 4);
	CHECK_EQ(counter.writeLog[0], 7);
	CHECK_EQ(counter.writeLog[1], 12);
	CHECK_EQ(counter.writeLog[2], 40);
	CHECK_EQ(counter.writeLog[3], 300);
}

static void test_write_through_mode() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 2, true);
	CHECK(cache.isWriteThrough());
	uint8_t buffer[512];
	sectorWith(buffer, 0x11);
	CHECK_EQ(cache.write(buffer, 3), 0);
	CHECK_EQ(counter.writes, 1);
	CHECK(ramHas(ram, 3, 0x11));
	CHECK_EQ(cache.getDirtyCount(), 0);
	// The written sector is cached for reading.
	CHECK_EQ(cache.read(buffer, 3), 0);
	CHECK_EQ(counter.reads, 0);
	// Eviction never writes in write-through mode.
	CHECK_EQ(cache.read(buffer, 4), 0);
	CHECK_EQ(cache.read(buffer, 5), 0);
	CHECK_EQ(counter.writes, 1);
}

static void test_read_error_passes_through() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 2);
	uint8_t buffer[512];
	sectorWith(buffer, 0x22);
	CHECK_EQ(ram.write(buffer, 8), 0);

	counter.failReadSector = 8;
	CHECK_EQ(cache.read(buffer, 8), SectorDevice_ioError);
	CHECK(!cache.isCached(8)); // a failed read must not leave garbage cached
	counter.failReadSector = CountingSectorDevice::NO_SECTOR;
	memset(buffer, 0, sizeof(buffer));
	CHECK_EQ(cache.read(buffer, 8), 0);
	CHECK_EQ(buffer[0], 0x22);
}

static void test_failed_write_back_keeps_sector_dirty() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 1);
	uint8_t buffer[512];
	sectorWith(buffer, 0x33);
	CHECK_EQ(cache.write(buffer, 1), 0);

	// Evicting sector 1 fails: the read that needed the slot reports the error.
	counter.failWriteSector = 1;
	CHECK_EQ(cache.read(buffer, 2), SectorDevice_ioError);
	CHECK(cache.isCached(1));
	CHECK_EQ(cache.getDirtyCount(), 1);
	CHECK_EQ(cache.flush(), SectorDevice_ioError);
	CHECK_EQ(cache.getDirtyCount(), 1);
	CHECK_EQ(counter.flushes, 0); // device not flushed while data is missing

	// Once the device works again nothing has been lost.
	counter.failWriteSector = CountingSectorDevice::NO_SECTOR;
	CHECK_EQ(cache.flush(), 0);
	CHECK(ramHas(ram, 1, 0x33));
	CHECK_EQ(cache.getDirtyCount(), 0);
}

static void test_flush_continues_past_failed_sector() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 3);
	uint8_t buffer[512];
	sectorWith(buffer, 0x44);
	CHECK_EQ(cache.write(buffer, 1), 0);
	CHECK_EQ(cache.write(buffer, 2), 0);
	CHECK_EQ(cache.write(buffer, 3), 0);
	counter.failWriteSector = 2;
	CHECK_EQ(cache.flush(), SectorDevice_ioError);
	CHECK(ramHas(ram, 1, 0x44));
	CHECK(ramHas(ram, 3, 0x44));
	CHECK(!ramHas(ram, 2, 0x44));
	CHECK_EQ(cache.getDirtyCount(), 1);
	counter.failWriteSector = CountingSectorDevice::NO_SECTOR;
	CHECK_EQ(cache.flush(), 0);
	CHECK(ramHas(ram, 2, 0x44));
}

static void test_write_through_error() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 2, true);
	uint8_t buffer[512];
	sectorWith(buffer, 0x55);
	CHECK_EQ(cache.write(buffer, 6), 0);
	counter.failWriteSector = 6;
	sectorWith(buffer, 0x66);
	CHECK_EQ(cache.write(buffer, 6), SectorDevice_ioError);
	// The cache must not claim the failed data is on the device.
	counter.failWriteSector = CountingSectorDevice::NO_SECTOR;
	CHECK(!cache.isCached(6));
	CHECK_EQ(cache.read(buffer, 6), 0);
	CHECK_EQ(buffer[0], 0x55);
}

static void test_invalidate_discards() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	{
		CachedSectorDevice cache(counter, 2);
		uint8_t buffer[512];
		sectorWith(buffer, 0x77);
		CHECK_EQ(ram.write(buffer, 9), 0);
		CHECK_EQ(cache.read(buffer, 9), 0);
		// Someone swaps the card: the device now has different data.
		sectorWith(buffer, 0x78);
		CHECK_EQ(ram.write(buffer, 9), 0);
		sectorWith(buffer, 0x79);
		CHECK_EQ(cache.write(buffer, 4), 0); // dirty, will be dropped
		cache.invalidate();
		CHECK_EQ(cache.getDirtyCount(), 0);
		CHECK(!cache.isCached(9));
		CHECK_EQ(cache.read(buffer, 9), 0);
		CHECK_EQ(buffer[0], 0x78);
	}
	// Destroying the cache must not write the discarded sector.
	CHECK_EQ(counter.writes, 0);
	CHECK(!ramHas(ram, 4, 0x79));
}

static void test_destructor_flushes() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	{
		CachedSectorDevice cache(ram, 4);
		uint8_t buffer[512];
		sectorWith(buffer, 0x99);
		CHECK_EQ(cache.write(buffer, 15), 0);
		CHECK(!ramHas(ram, 15, 0x99));
	}
	CHECK(ramHas(ram, 15, 0x99));
}

static void test_errors_from_device_range() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CachedSectorDevice cache(ram, 2);
	uint8_t buffer[512];
	CHECK_EQ(cache.read(buffer, SMALL_DISK), SectorDevice_sectorOutOfRange);
	// Write-back of an out of range sector fails at flush time.
	sectorWith(buffer, 1);
	CHECK_EQ(cache.write(buffer, SMALL_DISK + 5), 0);
	CHECK_EQ(cache.flush(), SectorDevice_sectorOutOfRange);
	cache.invalidate();
}

static void test_flush_through_partition_and_fat() {
	MemorySectorDevice ram(VOLUME_SECTORS, g_config.ramDiskSectors);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 4);
	Partition partition(cache);
	CHECK_OK(partition.open(Partition::WHOLE_DEVICE));
	CHECK_OK(formatFat32(partition, VOLUME_SECTORS, 1));
	Fat fat(partition);
	CHECK_OK(fat.mount());
	{
		File file(fat);
		CHECK_OK(file.open("/", "A.TXT", true));
		CHECK_OK(file.write(reinterpret_cast<const uint8_t*>("hello"), 5));
		CHECK(cache.getDirtyCount() > 0);
		CHECK_OK(file.flush());
		CHECK_EQ(cache.getDirtyCount(), 0);
		CHECK(counter.flushes > 0);
	}
}

/** Run a typical logging workload on fat. @return false if anything failed. */
static bool workload(Fat &fat) {
	// Many small appends...
	{
		File log(fat);
		if (log.open("/", "LOG.TXT", true)) return false;
		uint8_t line[24];
		for (uint32_t i = 0; i < 400; i++) {
			fillPattern(line, sizeof(line), i * sizeof(line), 5);
			if (log.write(line, sizeof(line))) return false;
		}
		if (log.close()) return false;
	}
	// ...and walking a long cluster chain (a 50KB file, 100 clusters) twice.
	if (writeFile(fat, "/", "BIG.BIN", 50000, 6, 700)) return false;
	bool ok = verifyFile(fat, "/", "BIG.BIN", 50000, 6, 700);
	ok = verifyFile(fat, "/", "BIG.BIN", 50000, 6, 700) && ok;
	return verifyFile(fat, "/", "LOG.TXT", 400 * 24, 5, 100) && ok;
}

static void test_cache_reduces_device_io() {
	if (g_config.ramDiskSectors < 256) {
		TestFramework::skip("needs a RAM disk of at least 256 sectors");
		return;
	}
	uint32_t directReads, directWrites;
	{
		MemorySectorDevice ram(VOLUME_SECTORS, g_config.ramDiskSectors);
		CountingSectorDevice counter(ram);
		CHECK_OK(formatFat32(counter, VOLUME_SECTORS, 1));
		Fat fat(counter);
		CHECK_OK(fat.mount());
		counter.resetCounts();
		CHECK(workload(fat));
		directReads = counter.reads;
		directWrites = counter.writes;
	}
	uint32_t cachedReads, cachedWrites;
	{
		MemorySectorDevice ram(VOLUME_SECTORS, g_config.ramDiskSectors);
		CountingSectorDevice counter(ram);
		CachedSectorDevice cache(counter, MFS_BLOCK_CACHE_DEFAULT_SLOTS);
		CHECK_OK(formatFat32(cache, VOLUME_SECTORS, 1));
		Fat fat(cache);
		CHECK_OK(fat.mount());
		CHECK_OK(cache.flush());
		counter.resetCounts();
		CHECK(workload(fat));
		CHECK_EQ(cache.flush(), 0);
		cachedReads = counter.reads;
		cachedWrites = counter.writes;
		printf("    device I/O for the workload: direct %lu reads / %lu writes, "
		       "%d slot cache %lu reads / %lu writes (hits %lu, misses %lu)\n",
		       (unsigned long)directReads, (unsigned long)directWrites, MFS_BLOCK_CACHE_DEFAULT_SLOTS,
		       (unsigned long)cachedReads, (unsigned long)cachedWrites,
		       (unsigned long)cache.getStats().hits, (unsigned long)cache.getStats().misses);
	}
	// The cache must at least halve both reads and writes for this workload.
	CHECK(cachedReads * 2 <= directReads);
	CHECK(cachedWrites * 2 <= directWrites);
}

/** What a mounted volume looks like after power is lost. */
static void test_power_loss() {
	MemorySectorDevice ram(VOLUME_SECTORS, g_config.ramDiskSectors);
	CountingSectorDevice storage(ram);
	{
		CachedSectorDevice cache(storage, 4);
		CHECK_OK(formatFat32(cache, VOLUME_SECTORS, 1));
		Fat fat(cache);
		CHECK_OK(fat.mount());

		// Durable: written and flushed.
		CHECK_OK(writeFile(fat, "/", "SAFE.BIN", 3000, 1, 700));
		{
			File file(fat);
			CHECK_OK(file.open("/", "FLUSHED.TXT", true));
			CHECK_OK(file.write(reinterpret_cast<const uint8_t*>("0123456789"), 10));
			CHECK_OK(file.flush());
			// Written after the flush: may or may not survive.
			CHECK_OK(file.write(reinterpret_cast<const uint8_t*>("abcdef"), 6));

			// Not flushed at all.
			File lost(fat);
			CHECK_OK(lost.open("/", "LOST.BIN", true));
			uint8_t buffer[512];
			for (uint32_t i = 0; i < 8; i++) {
				fillPattern(buffer, sizeof(buffer), i * 512, 2);
				CHECK_OK(lost.write(buffer, sizeof(buffer)));
			}
			CHECK(cache.getDirtyCount() > 0);

			// Power cut: from now on nothing reaches the storage, and whatever was
			// only in the cache is gone. (The destructors below try to flush and fail.)
			storage.failWriteSector = CountingSectorDevice::ANY_SECTOR;
			cache.invalidate();
		}
	}

	// After power returns the raw device must still mount...
	Fat fat(ram);
	CHECK_OK(fat.mount());
	CHECK(fat.isMounted());
	// ...everything flushed must be intact...
	CHECK(verifyFile(fat, "/", "SAFE.BIN", 3000, 1, 700));
	{
		File file(fat);
		CHECK_OK(file.open("/", "FLUSHED.TXT"));
		CHECK(file.length() == 10 || file.length() == 16);
		uint8_t text[10];
		uint32_t bytesRead;
		CHECK_OK(file.read(text, 10, bytesRead));
		CHECK_EQ(bytesRead, 10);
		CHECK(memcmp(text, "0123456789", 10) == 0);
	}
	// ...and the rest must at least be readable without errors.
	CHECK(countEntries(fat, "/") >= 2);
	{
		// It may or may not have a directory entry.
		File lost(fat);
		md_error_t error = lost.open("/", "LOST.BIN");
		if (error != File_fileNotFound) {
			CHECK_OK(error);
			uint8_t buffer[512];
			uint32_t bytesRead;
			do {
				CHECK_OK(lost.read(buffer, sizeof(buffer), bytesRead));
			} while (bytesRead > 0);
		}
	}
	uint32_t freeClusters;
	CHECK_OK(fat.countFreeClusters(freeClusters));
}

static void fillRam(MemorySectorDevice &ram, sector_t first, sector_t count) {
	uint8_t buffer[512];
	for (sector_t s = first; s < first + count; s++) {
		sectorWith(buffer, (uint8_t)(s + 1));
		CHECK_EQ(ram.write(buffer, s), 0);
	}
}

static void test_read_ahead_off_by_default() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 4);
	CHECK(!cache.isReadAhead());
	uint8_t buffer[512];
	CHECK_EQ(cache.read(buffer, 5), 0);
	CHECK_EQ(counter.reads, 1);
	CHECK(!cache.isCached(6));
	CHECK_EQ(cache.getStats().readAheads, 0);
}

static void test_read_ahead_stays_one_sector_ahead() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	fillRam(ram, 10, 6);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 4, false, true);
	uint8_t buffer[512];

	// A miss reads the sector and the next one
	CHECK_EQ(cache.read(buffer, 10), 0);
	CHECK_EQ(buffer[0], 11);
	CHECK_EQ(counter.reads, 2);
	CHECK(cache.isCached(11));

	// Each read of a read ahead sector is a hit that reads the one after
	for (sector_t s = 11; s < 15; s++) {
		CHECK_EQ(cache.read(buffer, s), 0);
		CHECK_EQ(buffer[511], (uint8_t)(s + 1));
		CHECK(cache.isCached(s + 1));
	}
	CHECK_EQ(counter.reads, 6);
	CHECK_EQ(cache.getStats().misses, 1);
	CHECK_EQ(cache.getStats().hits, 4);
	CHECK_EQ(cache.getStats().readAheads, 5);
	CHECK_EQ(cache.getStats().readAheadHits, 4);
	CHECK_EQ(cache.getStats().deviceReads, 6);

	// Reading a sector again is an ordinary hit, with no read ahead
	CHECK_EQ(cache.read(buffer, 14), 0);
	CHECK_EQ(counter.reads, 6);
}

static void test_read_ahead_one_slot() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	fillRam(ram, 20, 3);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 1, false, true);
	uint8_t buffer[512];
	// The read ahead takes the only slot, after the requested sector was copied out
	CHECK_EQ(cache.read(buffer, 20), 0);
	CHECK_EQ(buffer[0], 21);
	CHECK(!cache.isCached(20));
	CHECK(cache.isCached(21));
	CHECK_EQ(cache.read(buffer, 21), 0);
	CHECK_EQ(buffer[0], 22);
	CHECK(cache.isCached(22));
	CHECK_EQ(counter.reads, 3);
}

static void test_read_ahead_errors_ignored() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 4, false, true);
	uint8_t buffer[512];

	// The last sector: reading ahead past the end fails quietly
	CHECK_EQ(cache.read(buffer, SMALL_DISK - 1), 0);
	CHECK(!cache.isCached(SMALL_DISK));
	CHECK_EQ(cache.getStats().readAheads, 1);

	// A failing next sector doesn't fail the read, and isn't cached
	counter.failReadSector = 51;
	CHECK_EQ(cache.read(buffer, 50), 0);
	CHECK(!cache.isCached(51));
	CHECK(cache.read(buffer, 51) != 0);

	// A dirty sector that can't be written back isn't evicted for a read ahead
	CachedSectorDevice small(counter, 2, false, true);
	counter.failReadSector = CountingSectorDevice::NO_SECTOR;
	sectorWith(buffer, 0x5A);
	CHECK_EQ(small.write(buffer, 70), 0);
	counter.failWriteSector = 70;
	CHECK_EQ(small.read(buffer, 80), 0); // takes the free slot; reading 81 ahead would evict 70
	CHECK(small.isCached(70));
	CHECK(!small.isCached(81));
	CHECK_EQ(small.getDirtyCount(), 1);
	counter.failWriteSector = CountingSectorDevice::NO_SECTOR;
	CHECK_EQ(small.flush(), 0);
	CHECK(ramHas(ram, 70, 0x5A));
}

static void test_read_ahead_writes_back_evicted_sector() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 2, false, true);
	uint8_t buffer[512];
	sectorWith(buffer, 0x77);
	CHECK_EQ(cache.write(buffer, 30), 0);
	CHECK_EQ(cache.read(buffer, 40), 0); // reading 41 ahead evicts dirty 30
	CHECK(cache.isCached(41));
	CHECK(!cache.isCached(30));
	CHECK(ramHas(ram, 30, 0x77));
}

static void test_write_replaces_read_ahead_sector() {
	MemorySectorDevice ram(SMALL_DISK, 32);
	fillRam(ram, 60, 3);
	CountingSectorDevice counter(ram);
	CachedSectorDevice cache(counter, 4, false, true);
	uint8_t buffer[512];
	CHECK_EQ(cache.read(buffer, 60), 0);
	CHECK(cache.isCached(61));
	sectorWith(buffer, 0xC3);
	CHECK_EQ(cache.write(buffer, 61), 0);
	memset(buffer, 0, sizeof(buffer));
	CHECK_EQ(cache.read(buffer, 61), 0);
	CHECK_EQ(buffer[0], 0xC3);
	// Written data isn't a read ahead hit, so 62 isn't read
	CHECK(!cache.isCached(62));
	CHECK_EQ(cache.getStats().readAheadHits, 0);

	// Turning it off stops further read ahead
	cache.setReadAhead(false);
	CHECK_EQ(cache.read(buffer, 90), 0);
	CHECK(!cache.isCached(91));
}

const TestFramework::TestCase CACHE_TESTS[] = {
	{ "read_miss_then_hit", test_read_miss_then_hit },
	{ "lru_eviction_order", test_lru_eviction_order },
	{ "write_back_only_on_evict_or_flush", test_write_back_only_on_evict_or_flush },
	{ "flush_writes_in_sector_order", test_flush_writes_in_sector_order },
	{ "write_through_mode", test_write_through_mode },
	{ "read_error_passes_through", test_read_error_passes_through },
	{ "failed_write_back_keeps_sector_dirty", test_failed_write_back_keeps_sector_dirty },
	{ "flush_continues_past_failed_sector", test_flush_continues_past_failed_sector },
	{ "write_through_error", test_write_through_error },
	{ "invalidate_discards", test_invalidate_discards },
	{ "destructor_flushes", test_destructor_flushes },
	{ "errors_from_device_range", test_errors_from_device_range },
	{ "flush_through_partition_and_fat", test_flush_through_partition_and_fat },
	{ "cache_reduces_device_io", test_cache_reduces_device_io },
	{ "power_loss", test_power_loss },
	{ "read_ahead_off_by_default", test_read_ahead_off_by_default },
	{ "read_ahead_stays_one_sector_ahead", test_read_ahead_stays_one_sector_ahead },
	{ "read_ahead_one_slot", test_read_ahead_one_slot },
	{ "read_ahead_errors_ignored", test_read_ahead_errors_ignored },
	{ "read_ahead_writes_back_evicted_sector", test_read_ahead_writes_back_evicted_sector },
	{ "write_replaces_read_ahead_sector", test_write_replaces_read_ahead_sector },
};

const int CACHE_TEST_COUNT = (int)(sizeof(CACHE_TESTS) / sizeof(CACHE_TESTS[0]));

#endif
