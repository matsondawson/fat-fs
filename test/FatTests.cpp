/**
 * FAT driver tests. Portable: they run on the Linux host and on the Pico.
 *
 * Each test formats a fresh sparse RAM disk (a real size FAT32 volume that only
 * stores the sectors actually written).
 */

#include "TestEnv.h"

using namespace Mfs;

/******************************** Tests ********************************/

static void test_format_and_mount() {
	Volume v;
	CHECK_OK();
	CHECK(v.fat->isMounted());
	CHECK_EQ(v.fat->getFormat(), Format_FAT32);
	CHECK_EQ(v.fat->getBytesPerCluster(), CLUSTER_BYTES);
	CHECK(v.fat->getClusterCount() >= 65525);
	CHECK_EQ(countEntries(*v.fat, "/"), 0);
	// Everything is free except the root directory cluster.
	CHECK_EQ(v.fat->countFreeClusters(), v.fat->getClusterCount() - 1);
}

static void test_format_rejects_small_volume() {
	Disk disk(20000);
	SectorDevice &device = disk.device();
	formatFat32(device, 20000, 1);
	CHECK_EQ(mdError, Fat_formatVolumeTooSmall);
	formatFat32(device, VOLUME_SECTORS, 3);
	CHECK_EQ(mdError, Fat_formatInvalidClusterSize);
}

static void test_mount_unformatted_fails() {
	Disk disk(VOLUME_SECTORS);
	Fat fat(disk.device());
	CHECK(mdError != 0);
	CHECK(!fat.isMounted());

	File file(fat, "/", "A.TXT", true);
	CHECK_EQ(mdError, Fat_notMounted);
	CHECK(!file.isOpen());
}

static void test_short_names() {
	char out[11];
	CHECK(toShortName("test.txt", out));
	CHECK(memcmp(out, "TEST    TXT", 11) == 0);
	CHECK(toShortName("README", out));
	CHECK(memcmp(out, "README     ", 11) == 0);
	CHECK(toShortName("TEST    TXT", out));
	CHECK(memcmp(out, "TEST    TXT", 11) == 0);
	CHECK(toShortName("a.b", out));
	CHECK(memcmp(out, "A       B  ", 11) == 0);
	CHECK(toShortName("..", out));
	CHECK(memcmp(out, "..         ", 11) == 0);

	CHECK(!toShortName("", out));
	CHECK(!toShortName("toolongname.txt", out));
	CHECK(!toShortName("name.long", out));
	CHECK(!toShortName("bad*.txt", out));
	CHECK(!toShortName(".hidden", out));
	CHECK(!toShortName(" LEADING   ", out));
}

static void test_open_missing_file() {
	Volume v;
	CHECK_OK();
	File file(*v.fat, "/", "MISSING.TXT");
	CHECK_EQ(mdError, File_fileNotFound);
	CHECK(!file.isOpen());
	file.write((const uint8_t*)"x", 1);
	CHECK_EQ(mdError, File_notOpen);

	File bad(*v.fat, "/", "BAD*NAME.TXT", true);
	CHECK_EQ(mdError, File_invalidName);
}

static void test_create_write_read() {
	Volume v;
	CHECK_OK();
	{
		File file(*v.fat, "/", "HELLO.TXT", true);
		CHECK_OK();
		CHECK_EQ(file.length(), 0);
		file.write((const uint8_t*)"Hello, Pico!", 12);
		CHECK_OK();
		CHECK_EQ(file.length(), 12);
		CHECK_EQ(file.position(), 12);

		file.seek(0);
		CHECK_OK();
		char text[32] = {0};
		CHECK_EQ(file.read((uint8_t*)text, sizeof(text)), 12);
		CHECK(strcmp(text, "Hello, Pico!") == 0);
		CHECK_EQ(file.read((uint8_t*)text, sizeof(text)), 0); // EOF
	}
	// Reopen: data and size must have been persisted.
	{
		File file(*v.fat, "/", "hello.txt");
		CHECK_OK();
		CHECK_EQ(file.length(), 12);
		char text[13] = {0};
		CHECK_EQ(file.read((uint8_t*)text, 12), 12);
		CHECK(strcmp(text, "Hello, Pico!") == 0);
	}
	CHECK_EQ(countEntries(*v.fat, "/"), 1);
	CHECK_EQ(v.fat->countFreeClusters(), v.fat->getClusterCount() - 2);
}

static void test_create_existing_opens_it() {
	Volume v;
	CHECK_OK();
	writeFile(*v.fat, "/", "DATA.BIN", 100, 1, 100);
	CHECK_OK();
	File file(*v.fat, "/", "DATA.BIN", true);
	CHECK_OK();
	CHECK_EQ(file.length(), 100);
	CHECK_EQ(countEntries(*v.fat, "/"), 1);
}

static void test_empty_file_uses_no_clusters() {
	Volume v;
	CHECK_OK();
	uint32_t freeBefore = v.fat->countFreeClusters();
	File file(*v.fat, "/", "EMPTY.TXT", true);
	CHECK_OK();
	CHECK_EQ(v.fat->countFreeClusters(), freeBefore);
	uint8_t byte;
	CHECK_EQ(file.read(&byte, 1), 0);
	file.seek(0);
	CHECK_OK();
	file.seek(1);
	CHECK_EQ(mdError, File_seekExceedsFileSize);
}

static void test_multi_cluster_file() {
	Volume v;
	CHECK_OK();
	uint32_t freeBefore = v.fat->countFreeClusters();
	const uint32_t length = 5000; // 10 clusters, written and read in uneven chunks
	writeFile(*v.fat, "/", "BIG.BIN", length, 7, 700);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/", "BIG.BIN", length, 7, 333));
	CHECK(verifyFile(*v.fat, "/", "BIG.BIN", length, 7, 512));
	CHECK(verifyFile(*v.fat, "/", "BIG.BIN", length, 7, 1));
	CHECK_EQ(v.fat->countFreeClusters(), freeBefore - 10);
}

static void test_large_single_read() {
	Volume v;
	CHECK_OK();
	static uint8_t buffer[4096];
	fillPattern(buffer, sizeof(buffer), 0, 3);
	{
		File file(*v.fat, "/", "BLOCK.BIN", true);
		file.write(buffer, sizeof(buffer)); // one call spanning 8 clusters
		CHECK_OK();
	}
	memset(buffer, 0, sizeof(buffer));
	File file(*v.fat, "/", "BLOCK.BIN");
	CHECK_EQ(file.read(buffer, sizeof(buffer)), sizeof(buffer));
	CHECK(matchesPattern(buffer, sizeof(buffer), 0, 3));
}

static void test_cluster_boundaries() {
	Volume v;
	CHECK_OK();
	uint32_t freeBefore = v.fat->countFreeClusters();
	static uint8_t buffer[CLUSTER_BYTES * 2 + 1];
	fillPattern(buffer, sizeof(buffer), 0, 9);
	{
		File file(*v.fat, "/", "EDGE.BIN", true);
		file.write(buffer, CLUSTER_BYTES);      // exactly one cluster
		CHECK_OK();
		CHECK_EQ(v.fat->countFreeClusters(), freeBefore - 1);
		file.write(buffer + CLUSTER_BYTES, CLUSTER_BYTES); // exactly two
		CHECK_OK();
		CHECK_EQ(v.fat->countFreeClusters(), freeBefore - 2);
		file.write(buffer + 2 * CLUSTER_BYTES, 1); // spills into a third
		CHECK_OK();
		CHECK_EQ(v.fat->countFreeClusters(), freeBefore - 3);
		CHECK_EQ(file.length(), sizeof(buffer));
	}
	CHECK(verifyFile(*v.fat, "/", "EDGE.BIN", sizeof(buffer), 9, CLUSTER_BYTES));

	// Seeking to a cluster boundary and reading across it.
	File file(*v.fat, "/", "EDGE.BIN");
	file.seek(CLUSTER_BYTES - 2);
	CHECK_OK();
	uint8_t four[4];
	CHECK_EQ(file.read(four, 4), 4);
	CHECK(matchesPattern(four, 4, CLUSTER_BYTES - 2, 9));
	file.seek(CLUSTER_BYTES * 2);
	CHECK_OK();
	CHECK_EQ(file.read(four, 4), 1);
	CHECK(matchesPattern(four, 1, CLUSTER_BYTES * 2, 9));
}

static void test_seek_and_overwrite() {
	Volume v;
	CHECK_OK();
	writeFile(*v.fat, "/", "OVER.BIN", 2000, 5, 2000);
	CHECK_OK();
	uint32_t freeBefore = v.fat->countFreeClusters();
	{
		File file(*v.fat, "/", "OVER.BIN");
		file.seek(700);
		CHECK_OK();
		uint8_t xs[100];
		memset(xs, 'X', sizeof(xs));
		file.write(xs, sizeof(xs));
		CHECK_OK();
		CHECK_EQ(file.position(), 800);
		CHECK_EQ(file.length(), 2000); // overwrite must not grow the file
		CHECK_EQ(v.fat->countFreeClusters(), freeBefore);

		file.seek(2001);
		CHECK_EQ(mdError, File_seekExceedsFileSize);
	}
	File file(*v.fat, "/", "OVER.BIN");
	static uint8_t buffer[2000];
	CHECK_EQ(file.read(buffer, sizeof(buffer)), 2000);
	CHECK(matchesPattern(buffer, 700, 0, 5));
	for (int i = 700; i < 800; i++) CHECK_EQ(buffer[i], 'X');
	CHECK(matchesPattern(buffer + 800, 1200, 800, 5));

	// Overwrite that runs past the end grows the file.
	file.seek(1990);
	file.write((const uint8_t*)"0123456789ABCDEFGHIJ", 20);
	CHECK_OK();
	CHECK_EQ(file.length(), 2010);
}

static void test_append_after_reopen() {
	Volume v;
	CHECK_OK();
	writeFile(*v.fat, "/", "LOG.TXT", 1000, 2, 1000);
	CHECK_OK();
	for (int round = 0; round < 3; round++) {
		File file(*v.fat, "/", "LOG.TXT");
		CHECK_OK();
		uint32_t length = file.length();
		file.seek(length);
		CHECK_OK();
		static uint8_t buffer[300];
		fillPattern(buffer, sizeof(buffer), length, 2);
		file.write(buffer, sizeof(buffer));
		CHECK_OK();
	}
	CHECK(verifyFile(*v.fat, "/", "LOG.TXT", 1900, 2, 256));
}

static void test_truncate() {
	Volume v;
	CHECK_OK();
	uint32_t freeAtStart = v.fat->countFreeClusters();
	writeFile(*v.fat, "/", "TRUNC.BIN", 3000, 4, 700); // 6 clusters
	CHECK_OK();
	CHECK_EQ(v.fat->countFreeClusters(), freeAtStart - 6);
	{
		File file(*v.fat, "/", "TRUNC.BIN");
		file.seek(1000);
		file.truncate();
		CHECK_OK();
		CHECK_EQ(file.length(), 1000);
	}
	CHECK_EQ(v.fat->countFreeClusters(), freeAtStart - 2);
	CHECK(verifyFile(*v.fat, "/", "TRUNC.BIN", 1000, 4, 100));

	// Truncate at an exact cluster boundary keeps exactly that many clusters.
	{
		File file(*v.fat, "/", "TRUNC.BIN");
		file.seek(CLUSTER_BYTES);
		file.truncate();
		CHECK_OK();
	}
	CHECK_EQ(v.fat->countFreeClusters(), freeAtStart - 1);

	// Truncate to zero releases every cluster; the file can then grow again.
	{
		File file(*v.fat, "/", "TRUNC.BIN");
		file.truncate();
		CHECK_OK();
		CHECK_EQ(file.length(), 0);
	}
	CHECK_EQ(v.fat->countFreeClusters(), freeAtStart);
	writeFile(*v.fat, "/", "TRUNC.BIN", 600, 8, 600);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/", "TRUNC.BIN", 600, 8, 600));
}

static void test_remove() {
	Volume v;
	CHECK_OK();
	uint32_t freeAtStart = v.fat->countFreeClusters();
	writeFile(*v.fat, "/", "KEEP.TXT", 100, 1, 100);
	writeFile(*v.fat, "/", "GONE.TXT", 2000, 1, 700);
	CHECK_OK();
	cluster_t goneCluster;
	uint32_t goneOffset;
	{
		DirectoryListing dl(*v.fat, "/");
		CHECK(dl.find("GONE.TXT"));
		goneCluster = dl.cluster();
		goneOffset = dl.clusterOffset();
	}
	{
		File file(*v.fat, "/", "GONE.TXT");
		file.remove();
		CHECK_OK();
		CHECK(!file.isOpen());
	}
	File gone(*v.fat, "/", "GONE.TXT");
	CHECK_EQ(mdError, File_fileNotFound);
	CHECK_EQ(countEntries(*v.fat, "/"), 1);
	CHECK_EQ(v.fat->countFreeClusters(), freeAtStart - 1);
	CHECK(verifyFile(*v.fat, "/", "KEEP.TXT", 100, 1, 100));

	// The freed directory slot is reused.
	File again(*v.fat, "/", "NEW.TXT", true);
	CHECK_OK();
	DirectoryListing dl(*v.fat, "/");
	CHECK(dl.find("NEW.TXT"));
	CHECK_EQ(dl.cluster(), goneCluster);
	CHECK_EQ(dl.clusterOffset(), goneOffset);
}

static void test_directory_grows() {
	Volume v;
	CHECK_OK();
	// 16 entries fit in a 512 byte cluster, so 40 files need 3 directory clusters.
	const int fileCount = 40;
	char name[13];
	for (int i = 0; i < fileCount; i++) {
		snprintf(name, sizeof(name), "F%02d.TXT", i);
		writeFile(*v.fat, "/", name, 10 + i, (uint32_t)i, 64);
		CHECK_OK();
	}
	CHECK_EQ(countEntries(*v.fat, "/"), fileCount);
	for (int i = 0; i < fileCount; i++) {
		snprintf(name, sizeof(name), "F%02d.TXT", i);
		CHECK(verifyFile(*v.fat, "/", name, 10 + i, (uint32_t)i, 64));
	}
	// 3 directory clusters (root's first one was already used) + 40 file clusters
	CHECK_EQ(v.fat->countFreeClusters(), v.fat->getClusterCount() - 3 - fileCount);
}

static void test_subdirectories() {
	Volume v;
	CHECK_OK();
	makeDirectory(*v.fat, "/", "SUB");
	CHECK_OK();
	makeDirectory(*v.fat, "/SUB", "DEEP");
	CHECK_OK();
	makeDirectory(*v.fat, "/", "sub");
	CHECK_EQ(mdError, File_alreadyExists);

	writeFile(*v.fat, "/SUB/DEEP", "NOTE.TXT", 1500, 11, 500);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/sub/deep", "note.txt", 1500, 11, 500));
	// The original API's 11 character path form still works.
	CHECK(verifyFile(*v.fat, "/SUB        /DEEP       ", "NOTE    TXT", 1500, 11, 500));

	CHECK_EQ(countEntries(*v.fat, "/"), 1);          // SUB
	CHECK_EQ(countEntries(*v.fat, "/SUB"), 3);       // . .. DEEP
	CHECK_EQ(countEntries(*v.fat, "/SUB/DEEP"), 3);  // . .. NOTE.TXT

	// ".." navigation, including back to the root (cluster 0 in "..").
	CHECK(verifyFile(*v.fat, "/SUB/DEEP/../DEEP", "NOTE.TXT", 1500, 11, 500));
	CHECK_EQ(countEntries(*v.fat, "/SUB/.."), 1);

	DirectoryListing missing(*v.fat, "/NOPE");
	CHECK_EQ(mdError, DirectoryListing_directoryNotFound);
	File asFile(*v.fat, "/", "SUB");
	CHECK_EQ(mdError, File_isDirectory);
	DirectoryListing fileAsDir(*v.fat, "/SUB/DEEP/NOTE.TXT");
	CHECK_EQ(mdError, DirectoryListing_directoryNotFound);
}

static void test_partition_table() {
	const sector_t start = 2048;
	Disk disk(start + VOLUME_SECTORS);
	SectorDevice &device = disk.device();

	// Build an MBR with one FAT32 (LBA) partition in slot 0.
	static uint8_t mbr[512];
	memset(mbr, 0, sizeof(mbr));
	uint8_t *entry = mbr + 446;
	entry[4] = 0x0C;
	entry[8] = (uint8_t)start;  entry[9] = (uint8_t)(start >> 8);
	entry[10] = 0;              entry[11] = 0;
	entry[12] = (uint8_t)VOLUME_SECTORS; entry[13] = (uint8_t)(VOLUME_SECTORS >> 8);
	entry[14] = (uint8_t)(VOLUME_SECTORS >> 16); entry[15] = 0;
	mbr[510] = 0x55;
	mbr[511] = 0xAA;
	CHECK_EQ(device.write(mbr, 0), 0);

	Partition partition(device, Partition::primary(0));
	CHECK_OK();
	CHECK_EQ(partition.getSectorOffset(), start);
	CHECK_EQ(partition.getSectorCount(), VOLUME_SECTORS);

	formatFat32(partition, partition.getSectorCount(), 1);
	CHECK_OK();
	{
		Fat fat(partition);
		CHECK_OK();
		writeFile(fat, "/", "PART.TXT", 900, 6, 900);
		CHECK_OK();
	}
	// Remount and check the MBR survived and the data is inside the partition.
	static uint8_t check[512];
	CHECK_EQ(device.read(check, 0), 0);
	CHECK(memcmp(check, mbr, sizeof(mbr)) == 0);
	Partition again(device, Partition::primary(0));
	Fat fat(again);
	CHECK_OK();
	CHECK(verifyFile(fat, "/", "PART.TXT", 900, 6, 900));

	// Out of range access is rejected by the partition.
	CHECK_EQ(again.read(check, VOLUME_SECTORS), Partition_sectorOutOfRange);

	// Empty partition slot.
	Partition empty(device, Partition::primary(1));
	CHECK_EQ(mdError, Partition_tableInvalid);
}

static void test_whole_device_without_partition_table() {
	Disk disk(VOLUME_SECTORS);
	Partition whole(disk.device(), Partition::WHOLE_DEVICE);
	CHECK_OK();
	formatFat32(whole, VOLUME_SECTORS, 1);
	CHECK_OK();
	Fat fat(whole);
	CHECK_OK();
	writeFile(fat, "/", "RAW.TXT", 50, 1, 50);
	CHECK(verifyFile(fat, "/", "RAW.TXT", 50, 1, 50));
}

static void test_device_errors_propagate() {
	// A RAM disk that can hold very few sectors fills up: the error must surface.
	Disk disk(VOLUME_SECTORS, 12);
	formatFat32(disk.device(), VOLUME_SECTORS, 1);
	CHECK_OK();
	disk.device().flush();
	Fat fat(disk.device());
	CHECK_OK();
	File file(fat, "/", "FULL.BIN", true);
	CHECK_OK();
	static uint8_t buffer[512];
	fillPattern(buffer, sizeof(buffer), 0, 1);
	md_error_t error = 0;
	for (int i = 0; i < 20 && !error; i++) {
		file.write(buffer, sizeof(buffer));
		error = mdError;
	}
	if (!error) {
		// Behind a write-back cache the error appears when data reaches the device.
		file.flush();
		error = mdError;
	}
	CHECK_EQ(error, SectorDevice_storageFull);
}

static void test_multiple_open_files() {
	Volume v;
	CHECK_OK();
	{
		File a(*v.fat, "/", "A.BIN", true);
		File b(*v.fat, "/", "B.BIN", true);
		File c(*v.fat, "/", "C.BIN", true);
		CHECK_OK();
		// Interleaved writes make the three cluster chains interleave on disk.
		uint8_t chunk[200];
		for (uint32_t i = 0; i < 10; i++) {
			fillPattern(chunk, sizeof(chunk), i * 200, 1); a.write(chunk, sizeof(chunk));
			fillPattern(chunk, sizeof(chunk), i * 200, 2); b.write(chunk, sizeof(chunk));
			fillPattern(chunk, sizeof(chunk), i * 200, 3); c.write(chunk, sizeof(chunk));
			CHECK_OK();
		}
	}
	CHECK(verifyFile(*v.fat, "/", "A.BIN", 2000, 1, 333));
	CHECK(verifyFile(*v.fat, "/", "B.BIN", 2000, 2, 333));
	CHECK(verifyFile(*v.fat, "/", "C.BIN", 2000, 3, 333));
}

static void test_reader_and_writer_on_same_file() {
	Volume v;
	CHECK_OK();
	writeFile(*v.fat, "/", "SHARED.BIN", 1500, 4, 500);
	File reader(*v.fat, "/", "SHARED.BIN");
	File writer(*v.fat, "/", "SHARED.BIN");
	CHECK_OK();
	uint8_t xs[10];
	memset(xs, 'X', sizeof(xs));
	writer.seek(600);
	writer.write(xs, sizeof(xs)); // overwrite within the existing clusters
	CHECK_OK();
	reader.seek(595);
	uint8_t got[20];
	CHECK_EQ(reader.read(got, sizeof(got)), 20);
	CHECK(matchesPattern(got, 5, 595, 4));
	for (int i = 5; i < 15; i++) CHECK_EQ(got[i], 'X');
	CHECK(matchesPattern(got + 15, 5, 610, 4));
}

static void test_large_clusters() {
	// 4KB and 32KB clusters: volumes big enough for 65525 clusters (sparse RAM disk).
	static const uint8_t sizes[] = { 8, 64 };
	for (unsigned s = 0; s < sizeof(sizes); s++) {
		uint8_t spc = sizes[s];
		Volume v(spc, (sector_t)(66000u * spc + 2048));
		CHECK_OK();
		CHECK_EQ(v.fat->getBytesPerCluster(), spc * 512u);
		uint32_t freeBefore = v.fat->countFreeClusters();
		uint32_t length = spc * 512u + 700; // spans a cluster boundary
		writeFile(*v.fat, "/", "BIG.BIN", length, spc, 700);
		CHECK_OK();
		CHECK(verifyFile(*v.fat, "/", "BIG.BIN", length, spc, 512));
		CHECK_EQ(v.fat->countFreeClusters(), freeBefore - 2);
		File file(*v.fat, "/", "BIG.BIN");
		file.seek(spc * 512u - 1);
		CHECK_OK();
		uint8_t two[2];
		CHECK_EQ(file.read(two, 2), 2);
		CHECK(matchesPattern(two, 2, spc * 512u - 1, spc));
	}
}

static void test_remount_persists_everything() {
	Volume v;
	CHECK_OK();
	makeDirectory(*v.fat, "/", "DIR");
	writeFile(*v.fat, "/DIR", "ONE.BIN", 3000, 1, 700);
	writeFile(*v.fat, "/", "TWO.BIN", 10, 2, 10);
	CHECK_OK();
	uint32_t freeBefore = v.fat->countFreeClusters();
	v.remount();
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/DIR", "ONE.BIN", 3000, 1, 700));
	CHECK(verifyFile(*v.fat, "/", "TWO.BIN", 10, 2, 10));
	CHECK_EQ(v.fat->countFreeClusters(), freeBefore);
}

static void test_flush_persists_size() {
	Volume v;
	CHECK_OK();
	File file(*v.fat, "/", "GROW.TXT", true);
	// The first write gives the file its first cluster, so the entry (with the
	// size at that moment) is stored straight away.
	file.write((const uint8_t*)"abc", 3);
	CHECK_OK();
	{
		File other(*v.fat, "/", "GROW.TXT");
		CHECK_EQ(other.length(), 3);
	}
	// Later writes only update the size in memory until flush.
	file.write((const uint8_t*)"def", 3);
	CHECK_OK();
	{
		File other(*v.fat, "/", "GROW.TXT");
		CHECK_EQ(other.length(), 3);
	}
	file.flush();
	CHECK_OK();
	{
		File other(*v.fat, "/", "GROW.TXT");
		CHECK_EQ(other.length(), 6);
	}
	file.write((const uint8_t*)"ghi", 3);
	file.close();
	CHECK_OK();
	CHECK(!file.isOpen());
	file.write((const uint8_t*)"x", 1);
	CHECK_EQ(mdError, File_notOpen);
	File reopened(*v.fat, "/", "GROW.TXT");
	CHECK_EQ(reopened.length(), 9);
}

static void test_first_cluster_written_immediately() {
	// Even without a flush, a file that got data is never left pointing at no cluster.
	Volume v;
	CHECK_OK();
	File file(*v.fat, "/", "NEW.BIN", true);
	file.write((const uint8_t*)"x", 1);
	CHECK_OK();
	DirectoryListing dl(*v.fat, "/");
	CHECK(dl.find("NEW.BIN"));
	CHECK(dl.directoryEntry()->DIR_FstClusLO != 0 || dl.directoryEntry()->DIR_FstClusHI != 0);
}

static void test_deleted_first_character_name() {
	// A name starting with byte 0xE5 is stored as 0x05 and must round trip.
#ifdef MFS_LONG_FILE_NAMES
	TestFramework::skip("with long file names, names are UTF-8 and 0xE5 alone is not valid");
	return;
#endif
	Volume v;
	CHECK_OK();
	const char name[] = "\xE5" "AB.TXT";
	writeFile(*v.fat, "/", name, 20, 1, 20);
	CHECK_OK();
	CHECK(verifyFile(*v.fat, "/", name, 20, 1, 20));
	DirectoryListing dl(*v.fat, "/");
	CHECK(dl.next());
	CHECK_EQ(dl.directoryEntry()->DIR_Name[0], 0x05);
	CHECK_EQ(countEntries(*v.fat, "/"), 1);
}

static void test_name_listing_format() {
	Volume v;
	CHECK_OK();
	writeFile(*v.fat, "/", "NOEXT", 1, 1, 1);
	writeFile(*v.fat, "/", "A.B", 1, 1, 1);
	DirectoryListing dl(*v.fat, "/");
	char name[32];
	CHECK(dl.next());
	dl.name(name, sizeof(name));
	CHECK(strcmp(name, "NOEXT") == 0);
	CHECK(dl.next());
	dl.name(name, sizeof(name));
	CHECK(strcmp(name, "A.B") == 0);

	// Windows marks all lower case 8.3 names with DIR_NTRes flags instead of a long name.
	uint8_t shortName[11];
	memcpy(shortName, "README  TXT", 11);
	CHECK_EQ(formatShortName(shortName, 0x18, name, sizeof(name)), 10);
	CHECK(strcmp(name, "readme.txt") == 0);
	formatShortName(shortName, 0x08, name, sizeof(name));
	CHECK(strcmp(name, "readme.TXT") == 0);
	formatShortName(shortName, 0, name, 5); // truncated to fit
	CHECK(strcmp(name, "READ") == 0);
}

const TestFramework::TestCase FAT_TESTS[] = {
	{ "format_and_mount", test_format_and_mount },
	{ "format_rejects_small_volume", test_format_rejects_small_volume },
	{ "mount_unformatted_fails", test_mount_unformatted_fails },
	{ "short_names", test_short_names },
	{ "open_missing_file", test_open_missing_file },
	{ "create_write_read", test_create_write_read },
	{ "create_existing_opens_it", test_create_existing_opens_it },
	{ "empty_file_uses_no_clusters", test_empty_file_uses_no_clusters },
	{ "multi_cluster_file", test_multi_cluster_file },
	{ "large_single_read", test_large_single_read },
	{ "cluster_boundaries", test_cluster_boundaries },
	{ "seek_and_overwrite", test_seek_and_overwrite },
	{ "append_after_reopen", test_append_after_reopen },
	{ "truncate", test_truncate },
	{ "remove", test_remove },
	{ "directory_grows", test_directory_grows },
	{ "subdirectories", test_subdirectories },
	{ "partition_table", test_partition_table },
	{ "whole_device_without_partition_table", test_whole_device_without_partition_table },
	{ "device_errors_propagate", test_device_errors_propagate },
	{ "multiple_open_files", test_multiple_open_files },
	{ "reader_and_writer_on_same_file", test_reader_and_writer_on_same_file },
	{ "large_clusters", test_large_clusters },
	{ "remount_persists_everything", test_remount_persists_everything },
	{ "flush_persists_size", test_flush_persists_size },
	{ "first_cluster_written_immediately", test_first_cluster_written_immediately },
	{ "deleted_first_character_name", test_deleted_first_character_name },
	{ "name_listing_format", test_name_listing_format },
};

const int FAT_TEST_COUNT = (int)(sizeof(FAT_TESTS) / sizeof(FAT_TESTS[0]));
