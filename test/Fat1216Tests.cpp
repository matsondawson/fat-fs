/**
 * FAT12 and FAT16 specifics: the fixed root directory, 12 and 16 bit FAT entries
 * (including FAT12 entries split across two sectors) and the formatter. Run with
 * g_config.format set to Format_FAT16 and then Format_FAT12; the rest of the suite
 * also runs on both.
 */
#include "TestEnv.h"

#ifdef MFS_FAT12_16

using namespace Mfs;

static const uint32_t ROOT_ENTRIES = 512;

static void test_root_directory_is_fixed() {
	Volume v;
	CHECK_OK();
	CHECK_EQ(v.fat->getRootDirectoryCluster(), Fat::FIXED_ROOT_CLUSTER);
	CHECK_EQ(v.fat->getClusterBytes(Fat::FIXED_ROOT_CLUSTER), ROOT_ENTRIES * 32);
	CHECK_EQ(v.fat->getClusterBytes(2), CLUSTER_BYTES);
	// It has no FAT entry or chain
	CHECK(v.fat->isEndOfClusterChain(v.fat->getEntry(Fat::FIXED_ROOT_CLUSTER)));
	CHECK_OK();
	uint32_t freeBefore = v.fat->countFreeClusters();
	CHECK_EQ(freeBefore, v.fat->getClusterCount());

	// A whole root directory of files, which need no clusters while empty
	char name[13];
	for (uint32_t i = 0; i < ROOT_ENTRIES; i++) {
		snprintf(name, sizeof(name), "F%03u.TXT", (unsigned)i);
		File file(*v.fat, "/", name, true);
		CHECK_OK();
		if (mdError) return;
	}
	CHECK_EQ(countEntries(*v.fat, "/"), (int)ROOT_ENTRIES);
	CHECK_EQ(v.fat->countFreeClusters(), freeBefore);

	// One more doesn't fit, and nothing changes
	{
		File extra(*v.fat, "/", "EXTRA.TXT", true);
		CHECK_EQ(mdError, Fat_rootDirectoryFull);
		CHECK(!extra.isOpen());
	}
	makeDirectory(*v.fat, "/", "NEWDIR");
	CHECK_EQ(mdError, Fat_rootDirectoryFull);
	CHECK_EQ(v.fat->countFreeClusters(), freeBefore); // the new directory's cluster was freed
	CHECK_EQ(countEntries(*v.fat, "/"), (int)ROOT_ENTRIES);

	// After a delete there's room for one
	{
		File victim(*v.fat, "/", "F100.TXT");
		CHECK_OK();
		victim.remove();
		CHECK_OK();
	}
	writeFile(*v.fat, "/", "EXTRA.TXT", 1000, 5, 300);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/", "EXTRA.TXT", 1000, 5, 300));
	CHECK(verifyFile(*v.fat, "/", "F511.TXT", 0, 0, 10));

	v.remount();
	CHECK_OK();
	CHECK_EQ(countEntries(*v.fat, "/"), (int)ROOT_ENTRIES);
	CHECK(verifyFile(*v.fat, "/", "EXTRA.TXT", 1000, 5, 300));
}

#ifdef MFS_LONG_FILE_NAMES
static void test_long_name_needs_room_in_root() {
	Volume v;
	CHECK_OK();
	char name[13];
	// Leave two free entries: enough for an 8.3 name, not for a long one
	for (uint32_t i = 0; i < ROOT_ENTRIES - 2; i++) {
		snprintf(name, sizeof(name), "F%03u.TXT", (unsigned)i);
		File file(*v.fat, "/", name, true);
		CHECK_OK();
		if (mdError) return;
	}
	{
		// 27 characters: 3 long name entries and the 8.3 entry
		File longName(*v.fat, "/", "A rather long file name.txt", true);
		CHECK_EQ(mdError, Fat_rootDirectoryFull);
	}
	// Nothing was written: no orphaned long name entries
	CHECK_EQ(countEntries(*v.fat, "/"), (int)(ROOT_ENTRIES - 2));
	CHECK_EQ(countUsedSlots(*v.fat, "/"), (int)(ROOT_ENTRIES - 2));
	{
		// 1 long name entry and the 8.3 entry fill it exactly
		File shortLong(*v.fat, "/", "Short.txt", true);
		CHECK_OK();
	}
	CHECK_EQ(countEntries(*v.fat, "/"), (int)(ROOT_ENTRIES - 1));
	{
		File full(*v.fat, "/", "LAST.TXT", true);
		CHECK_EQ(mdError, Fat_rootDirectoryFull);
	}
}
#endif

static void test_dotdot_of_top_level_directory_is_cluster_0() {
	Volume v;
	CHECK_OK();
	makeDirectory(*v.fat, "/", "SUB");
	CHECK_OK();
	DirectoryListing listing(*v.fat, "/SUB");
	CHECK_OK();
	CHECK(listing.find(".."));
	DirectoryEntry *entry = listing.directoryEntry();
	CHECK_EQ(entry->DIR_FstClusHI, 0);
	CHECK_EQ(entry->DIR_FstClusLO, 0);
	writeFile(*v.fat, "/SUB/..", "TOP.TXT", 100, 3, 100);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/", "TOP.TXT", 100, 3, 100));
	CHECK_EQ(countEntries(*v.fat, "/"), 2);
}

static void test_fat_entries_round_trip() {
	Volume v;
	CHECK_OK();
	// The first 1500 entries: a few FAT sectors, so it fits the Pico's RAM disks
	const uint32_t clusters = v.fat->getClusterCount() < 1500 ? v.fat->getClusterCount() : 1500;
	const uint32_t mask = g_config.format == Format_FAT12 ? 0xFFF : 0xFFFF;
	// Every entry a different value, so a write that touched a neighbour shows. On
	// FAT12 that includes the entries split across sectors (clusters 341, 682, 1023, 1365).
	for (cluster_t cluster = 2; cluster < clusters + 2; cluster++) {
		v.fat->setEntry(cluster, (cluster * 2654435761u >> 7) & mask);
		CHECK_OK();
	}
	v.remount();
	CHECK_OK();
	for (cluster_t cluster = 2; cluster < clusters + 2; cluster++) {
		cluster_t value = v.fat->getEntry(cluster);
		if (value != ((cluster * 2654435761u >> 7) & mask)) {
			CHECK_EQ(value, (cluster * 2654435761u >> 7) & mask);
			return;
		}
	}
	// Both FAT copies were written: reading the second FAT gives the same values
	uint8_t first[512], second[512];
	uint32_t fatSectors = g_config.format == Format_FAT12 ? (clusters + 2) * 3 / 2 / 512 : (clusters + 2) * 2 / 512;
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
	CHECK_OK();
	const uint32_t length = 800 * CLUSTER_BYTES + 77;
	writeFile(*v.fat, "/", "BIG.BIN", length, 9, 700);
	CHECK_OK();
	v.remount();
	CHECK(verifyFile(*v.fat, "/", "BIG.BIN", length, 9, 512));
	CHECK_EQ(v.fat->countFreeClusters(), v.fat->getClusterCount() - 801);
	{
		File file(*v.fat, "/", "BIG.BIN");
		file.seek(400 * CLUSTER_BYTES);
		file.truncate();
		CHECK_OK();
	}
	CHECK_EQ(v.fat->countFreeClusters(), v.fat->getClusterCount() - 400);
	CHECK(verifyFile(*v.fat, "/", "BIG.BIN", 400 * CLUSTER_BYTES, 9, 512));
}

static void test_format_sizes() {
	Disk disk(70000);
	SectorDevice &device = disk.device();

	// FAT12: under 4085 clusters
	formatFat(device, 3000, Format_FAT12, 1);
	CHECK_OK();
	{
		Fat fat(device);
		CHECK_OK();
		CHECK_EQ(fat.getFormat(), Format_FAT12);
	}
	formatFat(device, 10000, Format_FAT12, 1);
	CHECK_EQ(mdError, Fat_formatVolumeTooLarge);
	formatFat(device, 10000, Format_FAT12, 0); // picks 4 sectors per cluster
	CHECK_OK();
	{
		Fat fat(device);
		CHECK_EQ(fat.getFormat(), Format_FAT12);
		CHECK_EQ(fat.getBytesPerCluster(), 4 * 512u);
	}

	// FAT16: 4085 to 65524 clusters
	formatFat(device, 3000, Format_FAT16, 1);
	CHECK_EQ(mdError, Fat_formatVolumeTooSmall);
	formatFat(device, 70000, Format_FAT16, 1);
	CHECK_EQ(mdError, Fat_formatVolumeTooLarge);
	formatFat(device, 70000, Format_FAT16, 0); // picks 2 sectors per cluster
	CHECK_OK();
	{
		Fat fat(device);
		CHECK_OK();
		CHECK_EQ(fat.getFormat(), Format_FAT16);
		CHECK_EQ(fat.getBytesPerCluster(), 2 * 512u);
		CHECK_EQ(fat.countFreeClusters(), fat.getClusterCount());
	}
	formatFat(device, 70000, Format_FAT16, 3);
	CHECK_EQ(mdError, Fat_formatInvalidClusterSize);

	// FAT32 through formatFat too
	formatFat(device, 70000, Format_FAT32, 1);
	CHECK_OK();
	Fat fat(device);
	CHECK_EQ(fat.getFormat(), Format_FAT32);
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
