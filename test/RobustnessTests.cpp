/**
 * Robustness tests: corrupt volumes, a full disk, and a randomized test that
 * checks the driver against a simple in-memory model of the files.
 */
#include "TestEnv.h"

using namespace Mfs;

/** Patch bytes of a sector on the device. */
static md_error_t patchSector(SectorDevice &device, sector_t sector, uint32_t offset, const void *bytes, uint32_t length) {
	uint8_t buffer[512];
	if (md_error_t error = device.read(buffer, sector)) return error;
	memcpy(buffer + offset, bytes, length);
	return device.write(buffer, sector);
}

/** Set a FAT entry directly in the first FAT of a volume made by Volume, bypassing the driver. */
static md_error_t setRawFatEntry(SectorDevice &device, cluster_t cluster, uint32_t value) {
	switch (g_config.format) {
	case FatFormat::FAT16: {
		uint16_t entry = (uint16_t)value;
		return patchSector(device, 1 + (cluster >> 8), (cluster & 255) * 2, &entry, 2);
	}
	case FatFormat::FAT12: {
		// The entry's two bytes, either of which may be in the next sector
		uint32_t offset = cluster + (cluster >> 1);
		uint8_t pair[2];
		for (int i = 0; i < 2; i++) {
			uint8_t buffer[512];
			if (md_error_t error = device.read(buffer, 1 + (offset + i) / 512)) return error;
			pair[i] = buffer[(offset + i) % 512];
		}
		uint32_t old = pair[0] | (uint32_t)pair[1] << 8;
		uint32_t merged = (cluster & 1) ? (old & 0x000F) | ((value & 0xFFF) << 4) : (old & 0xF000) | (value & 0xFFF);
		for (int i = 0; i < 2; i++) {
			uint8_t byte = (uint8_t)(merged >> (8 * i));
			if (md_error_t error = patchSector(device, 1 + (offset + i) / 512, (offset + i) % 512, &byte, 1)) return error;
		}
		return 0;
	}
	default:
		return patchSector(device, RESERVED_SECTORS + (cluster >> 7), (cluster & 127) * 4, &value, 4);
	}
}

/** The FAT entry marking a bad cluster: not free, and not part of any chain. */
static uint32_t badCluster() {
	switch (g_config.format) {
	case FatFormat::FAT12: return 0xFF7;
	case FatFormat::FAT16: return 0xFFF7;
	default: return 0x0FFFFFF7;
	}
}

static cluster_t firstClusterOf(Fat &fat, const char *path, const char *name) {
	DirectoryListing dl(fat);
	if (dl.open(path) || !dl.find(name)) return 0;
	return (static_cast<uint32_t>(dl.directoryEntry().DIR_FstClusHI) << 16) | dl.directoryEntry().DIR_FstClusLO;
}

static void test_corrupt_boot_sector_is_rejected() {
	struct Corruption {
		uint32_t offset;
		uint8_t length;
		uint8_t bytes[4];
		md_error_t expected;
	};
	static const Corruption corruptions[] = {
		{ 0,   1, { 0x00 },       Fat_invalidDriveFormat_bs_jmpboot },
		{ 11,  2, { 0x00, 0x04 }, Fat_invalidDriveFormat_sector_size_unsupported },  // 1024 bytes/sector
		{ 13,  1, { 3 },          Fat_invalidDriveFormat_sectorsPerCluster },
		{ 13,  1, { 128 },        Fat_invalidDriveFormat_cluster_exceeds_32k },
		{ 14,  2, { 0, 0 },       Fat_invalidDriveFormat_reserved_sectors_is_zero },
		{ 16,  1, { 0 },          Fat_invalidDriveFormat_numfats_must_not_be_zero },
		{ 44,  4, { 1, 0, 0, 0 }, Fat_invalidDriveFormat_rootClusterIndex },
		{ 510, 2, { 0x55, 0x55 }, Fat_invalidDriveFormat_bootBlockEndSignatureNotAA55 },
		// Fewer than 65525 clusters makes it a FAT16 volume, which isn't supported.
		{ 32,  4, { 0x8E, 0x79, 0x00, 0x00 }, 0 },
		// Total sectors smaller than the FATs.
		{ 32,  4, { 0x00, 0x04, 0x00, 0x00 }, Fat_invalidDriveFormat_volumeTooSmall },
		// FAT size of 1 sector can't describe 68882 clusters.
		{ 36,  4, { 1, 0, 0, 0 },             Fat_invalidDriveFormat_fatTooSmall },
	};
	for (unsigned i = 0; i < sizeof(corruptions) / sizeof(corruptions[0]); i++) {
		const Corruption &c = corruptions[i];
		Disk disk(VOLUME_SECTORS);
		CHECK_OK(formatFat32(disk.device(), VOLUME_SECTORS, 1));
		CHECK_EQ(patchSector(disk.device(), 0, c.offset, c.bytes, c.length), 0);
		Fat fat(disk.device());
		md_error_t error = fat.mount();
		if (fat.isMounted() || error == 0 || (c.expected && error != c.expected)) {
			printf("    corruption #%u: mounted=%d error=%lu expected=%lu\n", i, fat.isMounted(),
			       static_cast<unsigned long>(error), static_cast<unsigned long>(c.expected));
			CHECK(false);
		}
		// Nothing may be done with an unmounted volume.
		DirectoryListing dl(fat);
		CHECK_EQ(dl.open("/"), Fat_notMounted);
		CHECK(!dl.next());
	}
}

static void test_unreadable_device() {
	Disk disk(10); // too small to hold anything, every read past sector 9 fails
	Fat fat(disk.device());
	CHECK(fat.mount() != 0);
	CHECK(!fat.isMounted());
}

static void test_cluster_loop_in_directory_is_detected() {
	if (g_config.ramDiskSectors < 1024) {
		TestFramework::skip("walks the whole volume; host only");
		return;
	}
	Volume v;
	CHECK_OK(v.error);
	// Fill a directory's single cluster so there is no end marker, then make the
	// cluster point at itself. FAT32's root directory is a chain; the fixed FAT12/16
	// one isn't, so there a subdirectory is used.
	const bool chainedRoot = rootDirectoryClusters() != 0;
	const char *path = chainedRoot ? "/" : "/LOOPDIR";
	int files = 16;
	if (!chainedRoot) {
		CHECK_OK(makeDirectory(*v.fat, "/", "LOOPDIR"));
		files -= 2; // "." and ".."
	}
	char name[13];
	for (int i = 0; i < files; i++) {
		snprintf(name, sizeof(name), "F%02d", i);
		File file(*v.fat);
		CHECK_OK(file.open(path, name, true));
	}
	CHECK_OK(v.fat->flush());
	cluster_t directory = chainedRoot ? v.fat->getRootDirectoryCluster() : firstClusterOf(*v.fat, "/", "LOOPDIR");
	CHECK_EQ(setRawFatEntry(v.disk.device(), directory, directory), 0);
	CHECK_OK(v.remount());

	int count;
	CHECK_EQ(countEntries(*v.fat, path, count), Chain_loopDetected);
	File missing(*v.fat);
	CHECK_EQ(missing.open(path, "NOPE"), Chain_loopDetected);
}

static void test_cluster_loop_in_file() {
	Volume v;
	CHECK_OK(v.error);
	CHECK_OK(writeFile(*v.fat, "/", "LOOP.BIN", 1500, 1, 500)); // 3 clusters
	CHECK_OK(v.fat->flush());
	cluster_t first = firstClusterOf(*v.fat, "/", "LOOP.BIN");
	CHECK(first != 0);
	cluster_t second, third;
	CHECK_OK(v.fat->getEntry(first, second));
	CHECK_OK(v.fat->getEntry(second, third));
	CHECK_EQ(setRawFatEntry(v.disk.device(), third, first), 0); // last -> first
	CHECK_OK(v.remount());

	// Reads stop at the file size, so the loop is harmless there.
	CHECK_OK(verifyFile(*v.fat, "/", "LOOP.BIN", 1500, 1, 500));
	// Deleting must terminate.
	File file(*v.fat);
	CHECK_OK(file.open("/", "LOOP.BIN"));
	CHECK_OK(file.remove());
	File gone(*v.fat);
	CHECK_EQ(gone.open("/", "LOOP.BIN"), File_fileNotFound);
}

static void test_disk_full() {
	if (g_config.ramDiskSectors < 2048) {
		TestFramework::skip("fills the whole FAT; host only");
		return;
	}
	Volume v;
	CHECK_OK(v.error);
	// Mark every cluster used except three, by writing the FAT directly.
	uint32_t clusters = v.fat->getClusterCount();
	if (g_config.format == FatFormat::FAT32) {
		// A sector at a time, as there are many
		uint32_t fatSectors = (clusters + 2 + 127) / 128;
		uint8_t buffer[512];
		for (uint32_t s = 0; s < fatSectors; s++) {
			CHECK_EQ(v.disk.device().read(buffer, RESERVED_SECTORS + s), 0);
			uint32_t *entries = (uint32_t*)buffer;
			for (uint32_t e = 0; e < 128; e++) {
				cluster_t cluster = s * 128 + e;
				if (cluster < 3 || cluster >= clusters + 2) continue; // reserved, root, past the end
				if (cluster >= 100 && cluster < 103) continue;       // the three left free
				entries[e] = badCluster();
			}
			CHECK_EQ(v.disk.device().write(buffer, RESERVED_SECTORS + s), 0);
		}
	} else {
		CHECK_OK(v.fat->flush());
		for (cluster_t cluster = 2; cluster < clusters + 2; cluster++) {
			if (cluster >= 100 && cluster < 103) continue;
			CHECK_EQ(setRawFatEntry(v.disk.device(), cluster, badCluster()), 0);
		}
	}
	CHECK_OK(v.remount());
	uint32_t freeClusters;
	CHECK_OK(v.fat->countFreeClusters(freeClusters));
	CHECK_EQ(freeClusters, 3);

	File file(*v.fat);
	CHECK_OK(file.open("/", "FULL.BIN", true));
	static uint8_t data[512 * 5];
	fillPattern(data, sizeof(data), 0, 4);
	CHECK_EQ(file.write(data, sizeof(data)), Fat_partitionIsFull);
	CHECK_EQ(file.length(), 512 * 3); // what fitted is kept
	CHECK_OK(file.close());
	CHECK_OK(v.fat->countFreeClusters(freeClusters));
	CHECK_EQ(freeClusters, 0);
	CHECK_OK(verifyFile(*v.fat, "/", "FULL.BIN", 512 * 3, 4, 512));

	CHECK_EQ(makeDirectory(*v.fat, "/", "NEWDIR"), Fat_partitionIsFull);

	// Freeing space makes it usable again.
	File again(*v.fat);
	CHECK_OK(again.open("/", "FULL.BIN"));
	CHECK_OK(again.seek(512));
	CHECK_OK(again.truncate());
	CHECK_OK(v.fat->countFreeClusters(freeClusters));
	CHECK_EQ(freeClusters, 2);
	CHECK_OK(makeDirectory(*v.fat, "/", "NEWDIR"));
}

/******************************** Model based random test ********************************/

static const uint32_t MODEL_FILES = 4;
static const uint32_t MODEL_MAX_LENGTH = 6000;

struct ModelFile {
	bool exists;
	uint32_t length;
	uint8_t data[MODEL_MAX_LENGTH];
};

static ModelFile g_model[MODEL_FILES];

static void modelName(uint32_t index, char *name, uint32_t size) {
#ifdef MFS_LONG_FILE_NAMES
	// Long names (with aliases that only differ in the numeric tail) when available.
	snprintf(name, size, "Model file number %lu.dat", (unsigned long)index);
#else
	snprintf(name, size, "MODEL%lu.DAT", (unsigned long)index);
#endif
}

static bool checkAgainstModel(Fat &fat, uint32_t index) {
	char name[40];
	modelName(index, name, sizeof(name));
	File file(fat);
	md_error_t error = file.open("/", name);
	if (!g_model[index].exists) return error == File_fileNotFound;
	if (error || file.length() != g_model[index].length) {
		printf("    %s: error %lu length %lu, model length %lu\n", name, static_cast<unsigned long>(error),
		       static_cast<unsigned long>(file.length()), static_cast<unsigned long>(g_model[index].length));
		return false;
	}
	static uint8_t buffer[MODEL_MAX_LENGTH];
	uint32_t bytesRead;
	if (file.read(buffer, MODEL_MAX_LENGTH, bytesRead) || bytesRead != g_model[index].length) return false;
	return memcmp(buffer, g_model[index].data, g_model[index].length) == 0;
}

static void test_random_operations_match_model() {
	Volume v;
	CHECK_OK(v.error);
	memset(g_model, 0, sizeof(g_model));
	Random random(12345);
	static uint8_t buffer[MODEL_MAX_LENGTH];
	char name[40];

	for (uint32_t step = 0; step < g_config.randomIterations; step++) {
		uint32_t index = random.below(MODEL_FILES);
		ModelFile &model = g_model[index];
		modelName(index, name, sizeof(name));
		uint32_t operation = random.below(10);

		if (operation <= 3) {
			// Write at a random position (possibly extending the file).
			File file(*v.fat);
			CHECK_OK(file.open("/", name, true));
			model.exists = true;
			uint32_t position = random.below(model.length + 1);
			uint32_t length = 1 + random.below(1500);
			if (position + length > MODEL_MAX_LENGTH) length = MODEL_MAX_LENGTH - position;
			for (uint32_t i = 0; i < length; i++) buffer[i] = (uint8_t)random.next();
			CHECK_OK(file.seek(position));
			CHECK_OK(file.write(buffer, length));
			memcpy(model.data + position, buffer, length);
			if (position + length > model.length) model.length = position + length;
		} else if (operation <= 5) {
			// Read a random range.
			File file(*v.fat);
			md_error_t error = file.open("/", name);
			if (!model.exists) {
				CHECK_EQ(error, File_fileNotFound);
				continue;
			}
			CHECK_OK(error);
			uint32_t position = random.below(model.length + 1);
			uint32_t length = random.below(2000);
			CHECK_OK(file.seek(position));
			uint32_t expected = model.length - position < length ? model.length - position : length;
			uint32_t bytesRead;
			CHECK_OK(file.read(buffer, length, bytesRead));
			CHECK_EQ(bytesRead, expected);
			CHECK(memcmp(buffer, model.data + position, expected) == 0);
		} else if (operation == 6) {
			// Truncate.
			if (!model.exists) continue;
			File file(*v.fat);
			CHECK_OK(file.open("/", name));
			uint32_t position = random.below(model.length + 1);
			CHECK_OK(file.seek(position));
			CHECK_OK(file.truncate());
			model.length = position;
		} else if (operation == 7) {
			// Delete.
			if (!model.exists) continue;
			File file(*v.fat);
			CHECK_OK(file.open("/", name));
			CHECK_OK(file.remove());
			model.exists = false;
			model.length = 0;
		} else if (operation == 8) {
			CHECK(checkAgainstModel(*v.fat, index));
		} else if (random.below(4) == 0) {
			CHECK_OK(v.remount());
		}
	}

	// Final state: every file matches, and no cluster has leaked.
	uint32_t usedClusters = 0;
	int existing = 0;
	for (uint32_t i = 0; i < MODEL_FILES; i++) {
		CHECK(checkAgainstModel(*v.fat, i));
		if (g_model[i].exists) existing++;
		usedClusters += (g_model[i].length + CLUSTER_BYTES - 1) / CLUSTER_BYTES;
	}
	CHECK_EQ(countEntries(*v.fat, "/"), existing);
	uint32_t directoryClusters = 0;
	{
		Chain chain(*v.fat);
		chain.init(v.fat->getRootDirectoryCluster());
		// The fixed FAT12/16 root directory takes no clusters
		directoryClusters = rootDirectoryClusters();
		bool moved = true;
		while (directoryClusters && moved) {
			CHECK_OK(chain.nextCluster(moved));
			if (moved) directoryClusters++;
		}
	}
	uint32_t freeClusters;
	CHECK_OK(v.fat->countFreeClusters(freeClusters));
	CHECK_EQ(freeClusters, v.fat->getClusterCount() - directoryClusters - usedClusters);
}

const TestFramework::TestCase ROBUSTNESS_TESTS[] = {
	{ "corrupt_boot_sector_is_rejected", test_corrupt_boot_sector_is_rejected },
	{ "unreadable_device", test_unreadable_device },
	{ "cluster_loop_in_directory_is_detected", test_cluster_loop_in_directory_is_detected },
	{ "cluster_loop_in_file", test_cluster_loop_in_file },
	{ "disk_full", test_disk_full },
	{ "random_operations_match_model", test_random_operations_match_model },
};

const int ROBUSTNESS_TEST_COUNT = (int)(sizeof(ROBUSTNESS_TESTS) / sizeof(ROBUSTNESS_TESTS[0]));
