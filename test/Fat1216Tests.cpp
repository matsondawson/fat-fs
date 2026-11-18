/**
 * FAT12 and FAT16 specifics: the fixed root directory, 12 and 16 bit FAT entries
 * (including FAT12 entries split across two sectors) and the formatter. Run with
 * g_config.format set to FatFormat::FAT16 and then FatFormat::FAT12; the rest of the suite
 * also runs on both.
 */
#include "TestEnv.h"

#ifdef MFS_FAT12_16

using namespace Mfs;

static const uint32_t ROOT_ENTRIES = 512;

static void test_root_directory_is_fixed() {
	Volume v;
	CHECK_OK(v.error);
	CHECK_EQ(v.fat->getRootDirectoryCluster(), Fat::FIXED_ROOT_CLUSTER);
	CHECK_EQ(v.fat->getClusterBytes(Fat::FIXED_ROOT_CLUSTER), ROOT_ENTRIES * 32);
	CHECK_EQ(v.fat->getClusterBytes(2), CLUSTER_BYTES);
	// It has no FAT entry or chain
	cluster_t next;
	CHECK_OK(v.fat->getEntry(Fat::FIXED_ROOT_CLUSTER, next));
	CHECK(v.fat->isEndOfClusterChain(next));
	uint32_t freeBefore;
	CHECK_OK(v.fat->countFreeClusters(freeBefore));
	CHECK_EQ(freeBefore, v.fat->getClusterCount());

	// A whole root directory of files, which need no clusters while empty
	char name[13];
	for (uint32_t i = 0; i < ROOT_ENTRIES; i++) {
		snprintf(name, sizeof(name), "F%03u.TXT", (unsigned)i);
		File file(*v.fat);
		CHECK_OK(file.open("/", name, true));
	}
	CHECK_EQ(countEntries(*v.fat, "/"), (int)ROOT_ENTRIES);
	uint32_t freeNow;
	CHECK_OK(v.fat->countFreeClusters(freeNow));
	CHECK_EQ(freeNow, freeBefore);

	// One more doesn't fit, and nothing changes
	{
		File extra(*v.fat);
		CHECK_EQ(extra.open("/", "EXTRA.TXT", true), Fat_rootDirectoryFull);
		CHECK(!extra.isOpen());
	}
	CHECK_EQ(makeDirectory(*v.fat, "/", "NEWDIR"), Fat_rootDirectoryFull);
	CHECK_OK(v.fat->countFreeClusters(freeNow));
	CHECK_EQ(freeNow, freeBefore); // the new directory's cluster was freed
	CHECK_EQ(countEntries(*v.fat, "/"), (int)ROOT_ENTRIES);

	// After a delete there's room for one
	{
		File victim(*v.fat);
		CHECK_OK(victim.open("/", "F100.TXT"));
		CHECK_OK(victim.remove());
	}
	CHECK_OK(writeFile(*v.fat, "/", "EXTRA.TXT", 1000, 5, 300));
	CHECK(verifyFile(*v.fat, "/", "EXTRA.TXT", 1000, 5, 300));
	CHECK(verifyFile(*v.fat, "/", "F511.TXT", 0, 0, 10));

	CHECK_OK(v.remount());
	CHECK_EQ(countEntries(*v.fat, "/"), (int)ROOT_ENTRIES);
	CHECK(verifyFile(*v.fat, "/", "EXTRA.TXT", 1000, 5, 300));
}

#ifdef MFS_LONG_FILE_NAMES
static void test_long_name_needs_room_in_root() {
	Volume v;
	CHECK_OK(v.error);
	char name[13];
	// Leave two free entries: enough for an 8.3 name, not for a long one
	for (uint32_t i = 0; i < ROOT_ENTRIES - 2; i++) {
		snprintf(name, sizeof(name), "F%03u.TXT", (unsigned)i);
		File file(*v.fat);
		CHECK_OK(file.open("/", name, true));
	}
	{
		// 27 characters: 3 long name entries and the 8.3 entry
		File longName(*v.fat);
		CHECK_EQ(longName.open("/", "A rather long file name.txt", true), Fat_rootDirectoryFull);
	}
	// Nothing was written: no orphaned long name entries
	CHECK_EQ(countEntries(*v.fat, "/"), (int)(ROOT_ENTRIES - 2));
	CHECK_EQ(countUsedSlots(*v.fat, "/"), (int)(ROOT_ENTRIES - 2));
	{
		// 1 long name entry and the 8.3 entry fill it exactly
		File shortLong(*v.fat);
		CHECK_OK(shortLong.open("/", "Short.txt", true));
	}
	CHECK_EQ(countEntries(*v.fat, "/"), (int)(ROOT_ENTRIES - 1));
	{
		File full(*v.fat);
		CHECK_EQ(full.open("/", "LAST.TXT", true), Fat_rootDirectoryFull);
	}
}
#endif

static void test_dotdot_of_top_level_directory_is_cluster_0() {
	Volume v;
	CHECK_OK(v.error);
	CHECK_OK(makeDirectory(*v.fat, "/", "SUB"));
	DirectoryListing listing(*v.fat);
	CHECK_OK(listing.open("/SUB"));
	CHECK(listing.find(".."));
	const DirectoryEntry &entry = listing.directoryEntry();
	CHECK_EQ(entry.DIR_FstClusHI, 0);
	CHECK_EQ(entry.DIR_FstClusLO, 0);
	CHECK_OK(writeFile(*v.fat, "/SUB/..", "TOP.TXT", 100, 3, 100));
	CHECK(verifyFile(*v.fat, "/", "TOP.TXT", 100, 3, 100));
	CHECK_EQ(countEntries(*v.fat, "/"), 2);
}

static void test_fat_entries_round_trip() {
	Volume v;
	CHECK_OK(v.error);
	// The first 1500 entries: a few FAT sectors, so it fits the Pico's RAM disks
	const uint32_t clusters = v.fat->getClusterCount() < 1500 ? v.fat->getClusterCount() : 1500;
	const uint32_t mask = g_config.format == FatFormat::FAT12 ? 0xFFF : 0xFFFF;
	// Every entry a different value, so a write that touched a neighbour shows. On
	// FAT12 that includes the entries split across sectors (clusters 341, 682, 1023, 1365).
	for (cluster_t cluster = 2; cluster < clusters + 2; cluster++) {
		CHECK_OK(v.fat->setEntry(cluster, (cluster * 2654435761u >> 7) & mask));
	}
	CHECK_OK(v.remount());
	for (cluster_t cluster = 2; cluster < clusters + 2; cluster++) {
		cluster_t value;
		CHECK_OK(v.fat->getEntry(cluster, value));
		if (value != ((cluster * 2654435761u >> 7) & mask)) {
			CHECK_EQ(value, (cluster * 2654435761u >> 7) & mask);
			return;
		}
	}
	// Both FAT copies were written: reading the second FAT gives the same values
	uint8_t first[512], second[512];
	uint32_t fatSectors = g_config.format == FatFormat::FAT12 ? (clusters + 2) * 3 / 2 / 512 : (clusters + 2) * 2 / 512;
	uint8_t boot[512];
	CHECK_EQ(v.disk.device().read(boot, 0), 0);
	uint32_t fatSize = boot[22] | boot[23] << 8;
	for (uint32_t s = 0; s < fatSectors; s++) {
		CHECK_EQ(v.disk.device().read(first, 1 + s), 0);
		CHECK_EQ(v.disk.device().read(second, 1 + fatSize + s), 0);
		CHECK(memcmp(first, second, 512) == 0);
	}
}

static void test_file_crossing_split_fat_entries() {
	// On FAT12, the entries of clusters 341 and 682 are split across two FAT sectors
	if (g_config.ramDiskSectors < 1024) {
		TestFramework::skip("writes 400 KB; host only");
		return;
	}
	Volume v;
	CHECK_OK(v.error);
	const uint32_t length = 800 * CLUSTER_BYTES + 77;
	CHECK_OK(writeFile(*v.fat, "/", "BIG.BIN", length, 9, 700));
	CHECK_OK(v.remount());
	CHECK(verifyFile(*v.fat, "/", "BIG.BIN", length, 9, 512));
	uint32_t freeClusters;
	CHECK_OK(v.fat->countFreeClusters(freeClusters));
	CHECK_EQ(freeClusters, v.fat->getClusterCount() - 801);
	{
		File file(*v.fat);
		CHECK_OK(file.open("/", "BIG.BIN"));
		CHECK_OK(file.seek(400 * CLUSTER_BYTES));
		CHECK_OK(file.truncate());
	}
	CHECK_OK(v.fat->countFreeClusters(freeClusters));
	CHECK_EQ(freeClusters, v.fat->getClusterCount() - 400);
	CHECK(verifyFile(*v.fat, "/", "BIG.BIN", 400 * CLUSTER_BYTES, 9, 512));
}

static void test_format_sizes() {
	Disk disk(70000);
	SectorDevice &device = disk.device();

	// FAT12: under 4085 clusters
	CHECK_OK(formatFat(device, 3000, FatFormat::FAT12, 1));
	{
		Fat fat(device);
		CHECK_OK(fat.mount());
		CHECK_EQ(fat.getFormat(), FatFormat::FAT12);
	}
	CHECK_EQ(formatFat(device, 10000, FatFormat::FAT12, 1), Fat_formatVolumeTooLarge);
	CHECK_OK(formatFat(device, 10000, FatFormat::FAT12, 0)); // picks 4 sectors per cluster
	{
		Fat fat(device);
		CHECK_OK(fat.mount());
		CHECK_EQ(fat.getFormat(), FatFormat::FAT12);
		CHECK_EQ(fat.getBytesPerCluster(), 4 * 512u);
	}

	// FAT16: 4085 to 65524 clusters
	CHECK_EQ(formatFat(device, 3000, FatFormat::FAT16, 1), Fat_formatVolumeTooSmall);
	CHECK_EQ(formatFat(device, 70000, FatFormat::FAT16, 1), Fat_formatVolumeTooLarge);
	CHECK_OK(formatFat(device, 70000, FatFormat::FAT16, 0)); // picks 2 sectors per cluster
	{
		Fat fat(device);
		CHECK_OK(fat.mount());
		CHECK_EQ(fat.getFormat(), FatFormat::FAT16);
		CHECK_EQ(fat.getBytesPerCluster(), 2 * 512u);
		uint32_t freeClusters;
		CHECK_OK(fat.countFreeClusters(freeClusters));
		CHECK_EQ(freeClusters, fat.getClusterCount());
	}
	CHECK_EQ(formatFat(device, 70000, FatFormat::FAT16, 3), Fat_formatInvalidClusterSize);

	// FAT32 through formatFat too
	CHECK_OK(formatFat(device, 70000, FatFormat::FAT32, 1));
	Fat fat(device);
	CHECK_OK(fat.mount());
	CHECK_EQ(fat.getFormat(), FatFormat::FAT32);
}

const TestFramework::TestCase FAT12_16_TESTS[] = {
	{ "root_directory_is_fixed", test_root_directory_is_fixed },
#ifdef MFS_LONG_FILE_NAMES
	{ "long_name_needs_room_in_root", test_long_name_needs_room_in_root },
#endif
	{ "dotdot_of_top_level_directory_is_cluster_0", test_dotdot_of_top_level_directory_is_cluster_0 },
	{ "fat_entries_round_trip", test_fat_entries_round_trip },
	{ "file_crossing_split_fat_entries", test_file_crossing_split_fat_entries },
	{ "format_sizes", test_format_sizes },
};

const int FAT12_16_TEST_COUNT = (int)(sizeof(FAT12_16_TESTS) / sizeof(FAT12_16_TESTS[0]));

#endif
