/**
 * FAT driver tests. Portable: they run on the Linux host and on the Pico.
 *
 * Each test formats a fresh sparse RAM disk (a real size FAT32 volume that only
 * stores the sectors actually written).
 */

#include "TestEnv.h"
#include "CountingSectorDevice.h"

using namespace Mfs;

/** Free cluster count, or 0xFFFFFFFF if counting failed. */
static uint32_t freeClusters(Fat &fat) {
	uint32_t count;
	return fat.countFreeClusters(count) ? 0xFFFFFFFFu : count;
}

/******************************** Tests ********************************/

static void test_format_and_mount() {
	Volume v;
	CHECK_OK(v.error);
	CHECK(v.fat->isMounted());
	CHECK_EQ(v.fat->getFormat(), g_config.format);
	CHECK_EQ(v.fat->getBytesPerCluster(), CLUSTER_BYTES);
	switch (g_config.format) {
	case FatFormat::FAT12: CHECK(v.fat->getClusterCount() < 4085); break;
	case FatFormat::FAT16: CHECK(v.fat->getClusterCount() >= 4085 && v.fat->getClusterCount() < 65525); break;
	default: CHECK(v.fat->getClusterCount() >= 65525); break;
	}
	CHECK_EQ(countEntries(*v.fat, "/"), 0);
	// Everything is free except the FAT32 root directory cluster.
	CHECK_EQ(freeClusters(*v.fat), v.fat->getClusterCount() - rootDirectoryClusters());
}

static void test_format_rejects_small_volume() {
	Disk disk(20000);
	SectorDevice &device = disk.device();
	CHECK_EQ(formatFat32(device, 20000, 1), Fat_formatVolumeTooSmall);
	CHECK_EQ(formatFat32(device, VOLUME_SECTORS, 3), Fat_formatInvalidClusterSize);
}

static void test_mount_unformatted_fails() {
	Disk disk(VOLUME_SECTORS);
	Fat fat(disk.device());
	CHECK(fat.mount() != 0);
	CHECK(!fat.isMounted());

	File file(fat);
	CHECK_EQ(file.open("/", "A.TXT", true), Fat_notMounted);
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
	CHECK_OK(v.error);
	File file(*v.fat);
	CHECK_EQ(file.open("/", "MISSING.TXT"), File_fileNotFound);
	CHECK(!file.isOpen());
	CHECK_EQ(file.write((const uint8_t*)"x", 1), File_notOpen);

	File bad(*v.fat);
	CHECK_EQ(bad.open("/", "BAD*NAME.TXT", true), File_invalidName);
}

static void test_create_write_read() {
	Volume v;
	CHECK_OK(v.error);
	{
		File file(*v.fat);
		CHECK_OK(file.open("/", "HELLO.TXT", true));
		CHECK_EQ(file.length(), 0);
		CHECK_OK(file.write((const uint8_t*)"Hello, Pico!", 12));
		CHECK_EQ(file.length(), 12);
		CHECK_EQ(file.position(), 12);

		CHECK_OK(file.seek(0));
		char text[32] = {0};
		uint32_t n;
		CHECK_OK(file.read(reinterpret_cast<uint8_t*>(text), sizeof(text), n));
		CHECK_EQ(n, 12);
		CHECK(strcmp(text, "Hello, Pico!") == 0);
		CHECK_OK(file.read(reinterpret_cast<uint8_t*>(text), sizeof(text), n));
		CHECK_EQ(n, 0); // EOF
	}
	// Reopen: data and size must have been persisted.
	{
		File file(*v.fat);
		CHECK_OK(file.open("/", "hello.txt"));
		CHECK_EQ(file.length(), 12);
		char text[13] = {0};
		uint32_t n;
		CHECK_OK(file.read(reinterpret_cast<uint8_t*>(text), 12, n));
		CHECK_EQ(n, 12);
		CHECK(strcmp(text, "Hello, Pico!") == 0);
	}
	CHECK_EQ(countEntries(*v.fat, "/"), 1);
	CHECK_EQ(freeClusters(*v.fat), v.fat->getClusterCount() - rootDirectoryClusters() - 1);
}

static void test_create_existing_opens_it() {
	Volume v;
	CHECK_OK(v.error);
	CHECK_OK(writeFile(*v.fat, "/", "DATA.BIN", 100, 1, 100));
	File file(*v.fat);
	CHECK_OK(file.open("/", "DATA.BIN", true));
	CHECK_EQ(file.length(), 100);
	CHECK_EQ(countEntries(*v.fat, "/"), 1);
}

static void test_empty_file_uses_no_clusters() {
	Volume v;
	CHECK_OK(v.error);
	uint32_t freeBefore = freeClusters(*v.fat);
	File file(*v.fat);
	CHECK_OK(file.open("/", "EMPTY.TXT", true));
	CHECK_EQ(freeClusters(*v.fat), freeBefore);
	uint8_t byte;
	uint32_t n;
	CHECK_OK(file.read(&byte, 1, n));
	CHECK_EQ(n, 0);
	CHECK_OK(file.seek(0));
	CHECK_EQ(file.seek(1), File_seekExceedsFileSize);
}

static void test_multi_cluster_file() {
	Volume v;
	CHECK_OK(v.error);
	uint32_t freeBefore = freeClusters(*v.fat);
	const uint32_t length = 5000; // 10 clusters, written and read in uneven chunks
	CHECK_OK(writeFile(*v.fat, "/", "BIG.BIN", length, 7, 700));
	CHECK_OK(verifyFile(*v.fat, "/", "BIG.BIN", length, 7, 333));
	CHECK_OK(verifyFile(*v.fat, "/", "BIG.BIN", length, 7, 512));
	CHECK_OK(verifyFile(*v.fat, "/", "BIG.BIN", length, 7, 1));
	CHECK_EQ(freeClusters(*v.fat), freeBefore - 10);
}

static void test_large_single_read() {
	Volume v;
	CHECK_OK(v.error);
	static uint8_t buffer[4096];
	fillPattern(buffer, sizeof(buffer), 0, 3);
	{
		File file(*v.fat);
		CHECK_OK(file.open("/", "BLOCK.BIN", true));
		CHECK_OK(file.write(buffer, sizeof(buffer))); // one call spanning 8 clusters
	}
	memset(buffer, 0, sizeof(buffer));
	File file(*v.fat);
	CHECK_OK(file.open("/", "BLOCK.BIN"));
	uint32_t n;
	CHECK_OK(file.read(buffer, sizeof(buffer), n));
	CHECK_EQ(n, sizeof(buffer));
	CHECK(matchesPattern(buffer, sizeof(buffer), 0, 3));
}

static void test_chain_reads_each_fat_sector_once() {
	// 20 one-sector clusters: their FAT entries are all in one FAT sector
	Disk disk(VOLUME_SECTORS);
	CountingSectorDevice counting(disk.device());
	Volume format; // only for its sizes
	CHECK_OK(format.error);
	const sector_t sectors = format.disk.ram.getSectorCount();
	CHECK_OK(formatFat(counting, sectors, g_config.format, 1, "TEST"));
	{
		Fat fat(counting);
		CHECK_OK(fat.mount());
		CHECK_OK(writeFile(fat, "/", "CHAIN.BIN", 20 * CLUSTER_BYTES, 4, CLUSTER_BYTES));
	}
	Fat fat(counting);
	CHECK_OK(fat.mount());
	File file(fat);
	CHECK_OK(file.open("/", "CHAIN.BIN"));
	counting.resetCounts();
	static uint8_t buffer[CLUSTER_BYTES];
	for (uint32_t i = 0; i < 20; i++) {
		uint32_t n;
		CHECK_OK(file.read(buffer, sizeof(buffer), n));
		CHECK_EQ(n, sizeof(buffer));
		CHECK(matchesPattern(buffer, n, i * CLUSTER_BYTES, 4));
	}
	// 20 data sectors and the one FAT sector, not one FAT read per cluster
	CHECK_EQ(counting.reads, 21);
}

// A RAM disk formatted as the pass's volume, seen through a CountingSectorDevice
struct CountedVolume {
	Disk disk;
	CountingSectorDevice counting;
	md_error_t error;

	CountedVolume() : disk(VOLUME_SECTORS), counting(disk.device()), error(0) {
		Volume sizes; // only for the sector count of this pass's format
		error = sizes.error ? sizes.error
		                    : formatFat(counting, sizes.disk.ram.getSectorCount(), g_config.format, 1, "TEST");
	}
};

static void test_small_reads_keep_the_fat_sector() {
	// Two files written a cluster each in turn, so neither's clusters follow one
	// another and every cluster needs a FAT lookup, all in the first FAT sector
	CountedVolume v;
	CHECK_OK(v.error);
	static uint8_t buffer[CLUSTER_BYTES];
	{
		Fat fat(v.counting);
		CHECK_OK(fat.mount());
		File a(fat), b(fat);
		CHECK_OK(a.open("/", "A.BIN", true));
		CHECK_OK(b.open("/", "B.BIN", true));
		for (uint32_t i = 0; i < 20; i++) {
			fillPattern(buffer, sizeof(buffer), i * CLUSTER_BYTES, 1);
			CHECK_OK(a.write(buffer, sizeof(buffer)));
			fillPattern(buffer, sizeof(buffer), i * CLUSTER_BYTES, 2);
			CHECK_OK(b.write(buffer, sizeof(buffer)));
		}
	}
	Fat fat(v.counting);
	CHECK_OK(fat.mount());
	File file(fat);
	CHECK_OK(file.open("/", "A.BIN"));
	v.counting.resetCounts();
	// 100 bytes at a time: every read goes through the volume's sector buffer
	uint32_t done = 0;
	for (;;) {
		uint32_t n;
		CHECK_OK(file.read(buffer, 100, n));
		if (n == 0) break;
		CHECK(matchesPattern(buffer, n, done, 1));
		done += n;
	}
	CHECK_EQ(done, 20 * CLUSTER_BYTES);
	// 103 reads, 19 of them crossing into the next sector: 122 data sector reads,
	// and the FAT sector once, not at each of the 19 clusters
	CHECK_EQ(v.counting.reads, 123);
}

static void test_files_read_in_turn_skip_the_fat() {
	if (g_config.ramDiskSectors < 1024) {
		TestFramework::skip("needs a RAM disk of at least 1024 sectors");
		return;
	}
	// Two files of 300 clusters each, one after the other on the disk, so their
	// FAT entries are in different FAT sectors
	CountedVolume v;
	CHECK_OK(v.error);
	const uint32_t clusters = 300;
	{
		Fat fat(v.counting);
		CHECK_OK(fat.mount());
		CHECK_OK(writeFile(fat, "/", "A.BIN", clusters * CLUSTER_BYTES, 1, CLUSTER_BYTES));
		CHECK_OK(writeFile(fat, "/", "B.BIN", clusters * CLUSTER_BYTES, 2, CLUSTER_BYTES));
	}
	Fat fat(v.counting);
	CHECK_OK(fat.mount());
	File a(fat), b(fat);
	CHECK_OK(a.open("/", "A.BIN"));
	CHECK_OK(b.open("/", "B.BIN"));
	v.counting.resetCounts();
	static uint8_t buffer[CLUSTER_BYTES];
	for (uint32_t i = 0; i < clusters; i++) {
		uint32_t n;
		CHECK_OK(a.read(buffer, sizeof(buffer), n));
		CHECK(n == sizeof(buffer) && matchesPattern(buffer, n, i * CLUSTER_BYTES, 1));
		CHECK_OK(b.read(buffer, sizeof(buffer), n));
		CHECK(n == sizeof(buffer) && matchesPattern(buffer, n, i * CLUSTER_BYTES, 2));
	}
	// Each file looks up the FAT once per FAT sector its chain covers (a few),
	// not at each of its 299 cluster boundaries
	CHECK(v.counting.reads >= 2 * clusters);
	CHECK(v.counting.reads <= 2 * clusters + 12);
}

static void test_cluster_boundaries() {
	Volume v;
	CHECK_OK(v.error);
	uint32_t freeBefore = freeClusters(*v.fat);
	static uint8_t buffer[CLUSTER_BYTES * 2 + 1];
	fillPattern(buffer, sizeof(buffer), 0, 9);
	{
		File file(*v.fat);
		CHECK_OK(file.open("/", "EDGE.BIN", true));
		CHECK_OK(file.write(buffer, CLUSTER_BYTES));      // exactly one cluster
		CHECK_EQ(freeClusters(*v.fat), freeBefore - 1);
		CHECK_OK(file.write(buffer + CLUSTER_BYTES, CLUSTER_BYTES)); // exactly two
		CHECK_EQ(freeClusters(*v.fat), freeBefore - 2);
		CHECK_OK(file.write(buffer + 2 * CLUSTER_BYTES, 1)); // spills into a third
		CHECK_EQ(freeClusters(*v.fat), freeBefore - 3);
		CHECK_EQ(file.length(), sizeof(buffer));
	}
	CHECK_OK(verifyFile(*v.fat, "/", "EDGE.BIN", sizeof(buffer), 9, CLUSTER_BYTES));

	// Seeking to a cluster boundary and reading across it.
	File file(*v.fat);
	CHECK_OK(file.open("/", "EDGE.BIN"));
	CHECK_OK(file.seek(CLUSTER_BYTES - 2));
	uint8_t four[4];
	uint32_t n;
	CHECK_OK(file.read(four, 4, n));
	CHECK_EQ(n, 4);
	CHECK(matchesPattern(four, 4, CLUSTER_BYTES - 2, 9));
	CHECK_OK(file.seek(CLUSTER_BYTES * 2));
	CHECK_OK(file.read(four, 4, n));
	CHECK_EQ(n, 1);
	CHECK(matchesPattern(four, 1, CLUSTER_BYTES * 2, 9));
}

static void test_seek_and_overwrite() {
	Volume v;
	CHECK_OK(v.error);
	CHECK_OK(writeFile(*v.fat, "/", "OVER.BIN", 2000, 5, 2000));
	uint32_t freeBefore = freeClusters(*v.fat);
	{
		File file(*v.fat);
		CHECK_OK(file.open("/", "OVER.BIN"));
		CHECK_OK(file.seek(700));
		uint8_t xs[100];
		memset(xs, 'X', sizeof(xs));
		CHECK_OK(file.write(xs, sizeof(xs)));
		CHECK_EQ(file.position(), 800);
		CHECK_EQ(file.length(), 2000); // overwrite must not grow the file
		CHECK_EQ(freeClusters(*v.fat), freeBefore);

		CHECK_EQ(file.seek(2001), File_seekExceedsFileSize);
	}
	File file(*v.fat);
	CHECK_OK(file.open("/", "OVER.BIN"));
	static uint8_t buffer[2000];
	uint32_t n;
	CHECK_OK(file.read(buffer, sizeof(buffer), n));
	CHECK_EQ(n, 2000);
	CHECK(matchesPattern(buffer, 700, 0, 5));
	for (int i = 700; i < 800; i++) CHECK_EQ(buffer[i], 'X');
	CHECK(matchesPattern(buffer + 800, 1200, 800, 5));

	// Overwrite that runs past the end grows the file.
	CHECK_OK(file.seek(1990));
	CHECK_OK(file.write((const uint8_t*)"0123456789ABCDEFGHIJ", 20));
	CHECK_EQ(file.length(), 2010);
}

static void test_append_after_reopen() {
	Volume v;
	CHECK_OK(v.error);
	CHECK_OK(writeFile(*v.fat, "/", "LOG.TXT", 1000, 2, 1000));
	for (int round = 0; round < 3; round++) {
		File file(*v.fat);
		CHECK_OK(file.open("/", "LOG.TXT"));
		uint32_t length = file.length();
		CHECK_OK(file.seek(length));
		static uint8_t buffer[300];
		fillPattern(buffer, sizeof(buffer), length, 2);
		CHECK_OK(file.write(buffer, sizeof(buffer)));
	}
	CHECK_OK(verifyFile(*v.fat, "/", "LOG.TXT", 1900, 2, 256));
}

static void test_truncate() {
	Volume v;
	CHECK_OK(v.error);
	uint32_t freeAtStart = freeClusters(*v.fat);
	CHECK_OK(writeFile(*v.fat, "/", "TRUNC.BIN", 3000, 4, 700)); // 6 clusters
	CHECK_EQ(freeClusters(*v.fat), freeAtStart - 6);
	{
		File file(*v.fat);
		CHECK_OK(file.open("/", "TRUNC.BIN"));
		CHECK_OK(file.seek(1000));
		CHECK_OK(file.truncate());
		CHECK_EQ(file.length(), 1000);
	}
	CHECK_EQ(freeClusters(*v.fat), freeAtStart - 2);
	CHECK_OK(verifyFile(*v.fat, "/", "TRUNC.BIN", 1000, 4, 100));

	// Truncate at an exact cluster boundary keeps exactly that many clusters.
	{
		File file(*v.fat);
		CHECK_OK(file.open("/", "TRUNC.BIN"));
		CHECK_OK(file.seek(CLUSTER_BYTES));
		CHECK_OK(file.truncate());
	}
	CHECK_EQ(freeClusters(*v.fat), freeAtStart - 1);

	// Truncate to zero releases every cluster; the file can then grow again.
	{
		File file(*v.fat);
		CHECK_OK(file.open("/", "TRUNC.BIN"));
		CHECK_OK(file.truncate());
		CHECK_EQ(file.length(), 0);
	}
	CHECK_EQ(freeClusters(*v.fat), freeAtStart);
	CHECK_OK(writeFile(*v.fat, "/", "TRUNC.BIN", 600, 8, 600));
	CHECK_OK(verifyFile(*v.fat, "/", "TRUNC.BIN", 600, 8, 600));
}

static void test_remove() {
	Volume v;
	CHECK_OK(v.error);
	uint32_t freeAtStart = freeClusters(*v.fat);
	CHECK_OK(writeFile(*v.fat, "/", "KEEP.TXT", 100, 1, 100));
	CHECK_OK(writeFile(*v.fat, "/", "GONE.TXT", 2000, 1, 700));
	cluster_t goneCluster;
	uint32_t goneOffset;
	{
		DirectoryListing dl(*v.fat);
		CHECK_OK(dl.open("/"));
		CHECK(dl.find("GONE.TXT"));
		goneCluster = dl.cluster();
		goneOffset = dl.clusterOffset();
	}
	{
		File file(*v.fat);
		CHECK_OK(file.open("/", "GONE.TXT"));
		CHECK_OK(file.remove());
		CHECK(!file.isOpen());
	}
	File gone(*v.fat);
	CHECK_EQ(gone.open("/", "GONE.TXT"), File_fileNotFound);
	CHECK_EQ(countEntries(*v.fat, "/"), 1);
	CHECK_EQ(freeClusters(*v.fat), freeAtStart - 1);
	CHECK_OK(verifyFile(*v.fat, "/", "KEEP.TXT", 100, 1, 100));

	// The freed directory slot is reused.
	File again(*v.fat);
	CHECK_OK(again.open("/", "NEW.TXT", true));
	DirectoryListing dl(*v.fat);
	CHECK_OK(dl.open("/"));
	CHECK(dl.find("NEW.TXT"));
	CHECK_EQ(dl.cluster(), goneCluster);
	CHECK_EQ(dl.clusterOffset(), goneOffset);
}

static void test_directory_grows() {
	Volume v;
	CHECK_OK(v.error);
	// 16 entries fit in a 512 byte cluster, so 40 files need 3 directory clusters.
	const int fileCount = 40;
	char name[13];
	for (int i = 0; i < fileCount; i++) {
		snprintf(name, sizeof(name), "F%02d.TXT", i);
		CHECK_OK(writeFile(*v.fat, "/", name, 10 + i, (uint32_t)i, 64));
	}
	CHECK_EQ(countEntries(*v.fat, "/"), fileCount);
	for (int i = 0; i < fileCount; i++) {
		snprintf(name, sizeof(name), "F%02d.TXT", i);
		CHECK_OK(verifyFile(*v.fat, "/", name, 10 + i, (uint32_t)i, 64));
	}
	// FAT32: 3 directory clusters (root's first one was already used) + 40 file clusters.
	// The FAT12/16 root directory has room for 512 entries, so it takes no clusters.
	const uint32_t directoryClusters = rootDirectoryClusters() ? 3 : 0;
	CHECK_EQ(freeClusters(*v.fat), v.fat->getClusterCount() - directoryClusters - fileCount);
}

static void test_subdirectories() {
	Volume v;
	CHECK_OK(v.error);
	CHECK_OK(makeDirectory(*v.fat, "/", "SUB"));
	CHECK_OK(makeDirectory(*v.fat, "/SUB", "DEEP"));
	CHECK_EQ(makeDirectory(*v.fat, "/", "sub"), File_alreadyExists);

	CHECK_OK(writeFile(*v.fat, "/SUB/DEEP", "NOTE.TXT", 1500, 11, 500));
	CHECK_OK(verifyFile(*v.fat, "/sub/deep", "note.txt", 1500, 11, 500));
	// The original API's 11 character path form still works.
	CHECK_OK(verifyFile(*v.fat, "/SUB        /DEEP       ", "NOTE    TXT", 1500, 11, 500));

	CHECK_EQ(countEntries(*v.fat, "/"), 1);          // SUB
	CHECK_EQ(countEntries(*v.fat, "/SUB"), 3);       // . .. DEEP
	CHECK_EQ(countEntries(*v.fat, "/SUB/DEEP"), 3);  // . .. NOTE.TXT

	// ".." navigation, including back to the root (cluster 0 in "..").
	CHECK_OK(verifyFile(*v.fat, "/SUB/DEEP/../DEEP", "NOTE.TXT", 1500, 11, 500));
	CHECK_EQ(countEntries(*v.fat, "/SUB/.."), 1);

	DirectoryListing missing(*v.fat);
	CHECK_EQ(missing.open("/NOPE"), DirectoryListing_directoryNotFound);
	File asFile(*v.fat);
	CHECK_EQ(asFile.open("/", "SUB"), File_isDirectory);
	DirectoryListing fileAsDir(*v.fat);
	CHECK_EQ(fileAsDir.open("/SUB/DEEP/NOTE.TXT"), DirectoryListing_directoryNotFound);
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

	Partition partition(device);
	CHECK_OK(partition.open(Partition::primary(0)));
	CHECK_EQ(partition.getSectorOffset(), start);
	CHECK_EQ(partition.getSectorCount(), VOLUME_SECTORS);

	CHECK_OK(formatFat32(partition, partition.getSectorCount(), 1));
	{
		Fat fat(partition);
		CHECK_OK(fat.mount());
		CHECK_OK(writeFile(fat, "/", "PART.TXT", 900, 6, 900));
	}
	// Remount and check the MBR survived and the data is inside the partition.
	static uint8_t check[512];
	CHECK_EQ(device.read(check, 0), 0);
	CHECK(memcmp(check, mbr, sizeof(mbr)) == 0);
	Partition again(device);
	CHECK_OK(again.open(Partition::primary(0)));
	Fat fat(again);
	CHECK_OK(fat.mount());
	CHECK_OK(verifyFile(fat, "/", "PART.TXT", 900, 6, 900));

	// Out of range access is rejected by the partition.
	CHECK_EQ(again.read(check, VOLUME_SECTORS), Partition_sectorOutOfRange);

	// Empty partition slot.
	Partition empty(device);
	CHECK_EQ(empty.open(Partition::primary(1)), Partition_tableInvalid);
}

static void test_whole_device_without_partition_table() {
	Disk disk(VOLUME_SECTORS);
	Partition whole(disk.device());
	CHECK_OK(whole.open(Partition::WHOLE_DEVICE));
	CHECK_OK(formatFat32(whole, VOLUME_SECTORS, 1));
	Fat fat(whole);
	CHECK_OK(fat.mount());
	CHECK_OK(writeFile(fat, "/", "RAW.TXT", 50, 1, 50));
	CHECK_OK(verifyFile(fat, "/", "RAW.TXT", 50, 1, 50));
}

static void test_device_errors_propagate() {
	// A RAM disk that can hold very few sectors fills up: the error must surface.
	Disk disk(VOLUME_SECTORS, 12);
	CHECK_OK(formatFat32(disk.device(), VOLUME_SECTORS, 1));
	CHECK_OK(disk.device().flush());
	Fat fat(disk.device());
	CHECK_OK(fat.mount());
	File file(fat);
	CHECK_OK(file.open("/", "FULL.BIN", true));
	static uint8_t buffer[512];
	fillPattern(buffer, sizeof(buffer), 0, 1);
	md_error_t error = 0;
	for (int i = 0; i < 20 && !error; i++) {
		error = file.write(buffer, sizeof(buffer));
	}
	if (!error) {
		// Behind a write-back cache the error appears when data reaches the device.
		error = file.flush();
	}
	CHECK_EQ(error, SectorDevice_storageFull);
}

static void test_multiple_open_files() {
	Volume v;
	CHECK_OK(v.error);
	{
		File a(*v.fat);
		File b(*v.fat);
		File c(*v.fat);
		CHECK_OK(a.open("/", "A.BIN", true));
		CHECK_OK(b.open("/", "B.BIN", true));
		CHECK_OK(c.open("/", "C.BIN", true));
		// Interleaved writes make the three cluster chains interleave on disk.
		uint8_t chunk[200];
		for (uint32_t i = 0; i < 10; i++) {
			fillPattern(chunk, sizeof(chunk), i * 200, 1); CHECK_OK(a.write(chunk, sizeof(chunk)));
			fillPattern(chunk, sizeof(chunk), i * 200, 2); CHECK_OK(b.write(chunk, sizeof(chunk)));
			fillPattern(chunk, sizeof(chunk), i * 200, 3); CHECK_OK(c.write(chunk, sizeof(chunk)));
		}
	}
	CHECK_OK(verifyFile(*v.fat, "/", "A.BIN", 2000, 1, 333));
	CHECK_OK(verifyFile(*v.fat, "/", "B.BIN", 2000, 2, 333));
	CHECK_OK(verifyFile(*v.fat, "/", "C.BIN", 2000, 3, 333));
}

static void test_file_opens_once() {
	Volume v;
	CHECK_OK(v.error);
	CHECK_OK(writeFile(*v.fat, "/", "SHARED.BIN", 1500, 4, 500));
	CHECK_OK(writeFile(*v.fat, "/", "OTHER.BIN", 10, 5, 10));
	File first(*v.fat);
	CHECK_OK(first.open("/", "SHARED.BIN"));
	{
		File second(*v.fat);
		CHECK_EQ(second.open("/", "shared.bin"), File_alreadyOpen);
		CHECK(!second.isOpen());
	}
	// Asking to create it opens nothing either
	File created(*v.fat);
	CHECK_EQ(created.open("/", "SHARED.BIN", true), File_alreadyOpen);
	CHECK(!created.isOpen());
	// The first is untouched by the failed opens
	CHECK(first.isOpen());
	uint8_t got[20];
	CHECK_OK(first.seek(595));
	uint32_t n;
	CHECK_OK(first.read(got, sizeof(got), n));
	CHECK_EQ(n, 20);
	CHECK(matchesPattern(got, 20, 595, 4));

	// Other files open as usual, and closing one in the middle of the list
	// leaves the others open
	{
		File other(*v.fat);
		CHECK_OK(other.open("/", "OTHER.BIN"));
		File third(*v.fat);
		CHECK_OK(third.open("/", "THIRD.BIN", true));
		CHECK_OK(other.close());
		File again(*v.fat);
		CHECK_OK(again.open("/", "OTHER.BIN"));
		File thirdAgain(*v.fat);
		CHECK_EQ(thirdAgain.open("/", "THIRD.BIN"), File_alreadyOpen);
	}
	File shared2(*v.fat);
	CHECK_EQ(shared2.open("/", "SHARED.BIN"), File_alreadyOpen);

	// Once closed, it opens again
	CHECK_OK(first.close());
	File reopened(*v.fat);
	CHECK_OK(reopened.open("/", "SHARED.BIN"));
	CHECK_EQ(reopened.length(), 1500);
}

static void test_removed_file_can_be_created_again() {
	Volume v;
	CHECK_OK(v.error);
	File file(*v.fat);
	CHECK_OK(file.open("/", "GONE.TXT", true));
	CHECK_OK(file.write((const uint8_t*)"abc", 3));
	CHECK_OK(file.remove());
	CHECK(!file.isOpen());
	// The new entry may take the freed slot the removed File pointed at
	File again(*v.fat);
	CHECK_OK(again.open("/", "GONE.TXT", true));
	CHECK_EQ(again.length(), 0);
}

static void test_unmount_closes_open_files() {
	Volume v;
	CHECK_OK(v.error);
	File file(*v.fat);
	CHECK_OK(file.open("/", "LOG.TXT", true));
	CHECK_OK(file.write((const uint8_t*)"0123456789", 10));
	CHECK_OK(file.seek(10));
	CHECK_OK(file.write((const uint8_t*)"abc", 3));
	// Unmounting closes the File, storing its size, and leaves it closed
	CHECK_OK(v.remount());
	CHECK(!file.isOpen());
	CHECK_EQ(file.write((const uint8_t*)"x", 1), File_notOpen);
	File reopened(*v.fat);
	CHECK_OK(reopened.open("/", "LOG.TXT"));
	CHECK_EQ(reopened.length(), 13);
	// file outlives the Fat it was opened on: its destructor must not touch it (ASan checks)
}

static void test_large_clusters() {
	// 4KB and 32KB clusters: volumes big enough for 65525 clusters (sparse RAM disk).
	static const uint8_t sizes[] = { 8, 64 };
	for (unsigned s = 0; s < sizeof(sizes); s++) {
		uint8_t spc = sizes[s];
		Volume v(spc, (sector_t)(66000u * spc + 2048));
		CHECK_OK(v.error);
		CHECK_EQ(v.fat->getBytesPerCluster(), spc * 512u);
		uint32_t freeBefore = freeClusters(*v.fat);
		uint32_t length = spc * 512u + 700; // spans a cluster boundary
		CHECK_OK(writeFile(*v.fat, "/", "BIG.BIN", length, spc, 700));
		CHECK_OK(verifyFile(*v.fat, "/", "BIG.BIN", length, spc, 512));
		CHECK_EQ(freeClusters(*v.fat), freeBefore - 2);
		File file(*v.fat);
		CHECK_OK(file.open("/", "BIG.BIN"));
		CHECK_OK(file.seek(spc * 512u - 1));
		uint8_t two[2];
		uint32_t n;
		CHECK_OK(file.read(two, 2, n));
		CHECK_EQ(n, 2);
		CHECK(matchesPattern(two, 2, spc * 512u - 1, spc));
	}
}

static void test_remount_persists_everything() {
	Volume v;
	CHECK_OK(v.error);
	CHECK_OK(makeDirectory(*v.fat, "/", "DIR"));
	CHECK_OK(writeFile(*v.fat, "/DIR", "ONE.BIN", 3000, 1, 700));
	CHECK_OK(writeFile(*v.fat, "/", "TWO.BIN", 10, 2, 10));
	uint32_t freeBefore = freeClusters(*v.fat);
	CHECK_OK(v.remount());
	CHECK_OK(verifyFile(*v.fat, "/DIR", "ONE.BIN", 3000, 1, 700));
	CHECK_OK(verifyFile(*v.fat, "/", "TWO.BIN", 10, 2, 10));
	CHECK_EQ(freeClusters(*v.fat), freeBefore);
}

// The size in a file's directory entry on the volume, as another program would see it
static uint32_t storedSize(Fat &fat, const char *path, const char *name) {
	DirectoryListing dl(fat);
	if (dl.open(path)) return 0xFFFFFFFFu;
	return dl.find(name) ? dl.directoryEntry().DIR_FileSize : 0xFFFFFFFFu;
}

static void test_flush_persists_size() {
	Volume v;
	CHECK_OK(v.error);
	File file(*v.fat);
	CHECK_OK(file.open("/", "GROW.TXT", true));
	// The first write gives the file its first cluster, so the entry (with the
	// size at that moment) is stored straight away.
	CHECK_OK(file.write((const uint8_t*)"abc", 3));
	CHECK_EQ(storedSize(*v.fat, "/", "GROW.TXT"), 3);
	// Later writes only update the size in memory until flush.
	CHECK_OK(file.write((const uint8_t*)"def", 3));
	CHECK_EQ(storedSize(*v.fat, "/", "GROW.TXT"), 3);
	CHECK_OK(file.flush());
	CHECK_EQ(storedSize(*v.fat, "/", "GROW.TXT"), 6);
	CHECK_OK(file.write((const uint8_t*)"ghi", 3));
	CHECK_OK(file.close());
	CHECK(!file.isOpen());
	CHECK_EQ(file.write((const uint8_t*)"x", 1), File_notOpen);
	File reopened(*v.fat);
	CHECK_OK(reopened.open("/", "GROW.TXT"));
	CHECK_EQ(reopened.length(), 9);
}

static void test_first_cluster_written_immediately() {
	// Even without a flush, a file that got data is never left pointing at no cluster.
	Volume v;
	CHECK_OK(v.error);
	File file(*v.fat);
	CHECK_OK(file.open("/", "NEW.BIN", true));
	CHECK_OK(file.write((const uint8_t*)"x", 1));
	DirectoryListing dl(*v.fat);
	CHECK_OK(dl.open("/"));
	CHECK(dl.find("NEW.BIN"));
	CHECK(dl.directoryEntry().DIR_FstClusLO != 0 || dl.directoryEntry().DIR_FstClusHI != 0);
}

static void test_deleted_first_character_name() {
	// A name starting with byte 0xE5 is stored as 0x05 and must round trip.
#ifdef MFS_LONG_FILE_NAMES
	TestFramework::skip("with long file names, names are UTF-8 and 0xE5 alone is not valid");
	return;
#endif
	Volume v;
	CHECK_OK(v.error);
	const char name[] = "\xE5" "AB.TXT";
	CHECK_OK(writeFile(*v.fat, "/", name, 20, 1, 20));
	CHECK_OK(verifyFile(*v.fat, "/", name, 20, 1, 20));
	DirectoryListing dl(*v.fat);
	CHECK_OK(dl.open("/"));
	CHECK(dl.next());
	CHECK_EQ(dl.directoryEntry().DIR_Name[0], 0x05);
	CHECK_EQ(countEntries(*v.fat, "/"), 1);
}

static void test_name_listing_format() {
	Volume v;
	CHECK_OK(v.error);
	CHECK_OK(writeFile(*v.fat, "/", "NOEXT", 1, 1, 1));
	CHECK_OK(writeFile(*v.fat, "/", "A.B", 1, 1, 1));
	DirectoryListing dl(*v.fat);
	CHECK_OK(dl.open("/"));
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
	{ "chain_reads_each_fat_sector_once", test_chain_reads_each_fat_sector_once },
	{ "small_reads_keep_the_fat_sector", test_small_reads_keep_the_fat_sector },
	{ "files_read_in_turn_skip_the_fat", test_files_read_in_turn_skip_the_fat },
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
	{ "file_opens_once", test_file_opens_once },
	{ "removed_file_can_be_created_again", test_removed_file_can_be_created_again },
	{ "unmount_closes_open_files", test_unmount_closes_open_files },
	{ "large_clusters", test_large_clusters },
	{ "remount_persists_everything", test_remount_persists_everything },
	{ "flush_persists_size", test_flush_persists_size },
	{ "first_cluster_written_immediately", test_first_cluster_written_immediately },
	{ "deleted_first_character_name", test_deleted_first_character_name },
	{ "name_listing_format", test_name_listing_format },
};

const int FAT_TEST_COUNT = (int)(sizeof(FAT_TESTS) / sizeof(FAT_TESTS[0]));
