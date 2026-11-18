#include "TestEnv.h"

TestConfig g_config = { 256, 0, false, false, 200, FatFormat::FAT32 };

Disk::Disk(sector_t sectors, uint32_t maxStored)
	: ram(sectors, maxStored ? maxStored : g_config.ramDiskSectors) {
#ifdef MFS_BLOCK_CACHE
	cache = g_config.cacheSlots ? new CachedSectorDevice(ram, g_config.cacheSlots, g_config.writeThrough, g_config.readAhead) : nullptr;
#endif
}

Disk::~Disk() {
#ifdef MFS_BLOCK_CACHE
	delete cache;
#endif
}

SectorDevice &Disk::device() {
#ifdef MFS_BLOCK_CACHE
	if (cache) return *cache;
#endif
	return ram;
}

uint32_t rootDirectoryClusters() {
	return g_config.format == FatFormat::FAT32 ? 1 : 0;
}

static sector_t volumeSectors(uint8_t sectorsPerCluster, sector_t sectors) {
	switch (g_config.format) {
	case FatFormat::FAT12: return static_cast<sector_t>(sectorsPerCluster) * 3000 + 100;
	case FatFormat::FAT16: return static_cast<sector_t>(sectorsPerCluster) * 20000 + 300;
	default: return sectors;
	}
}

Volume::Volume(uint8_t sectorsPerCluster, sector_t sectors)
	: disk(volumeSectors(sectorsPerCluster, sectors)), fat(nullptr), error(0) {
	error = formatFat(disk.device(), volumeSectors(sectorsPerCluster, sectors), g_config.format, sectorsPerCluster, "TEST");
	if (error) return;
	fat = new Fat(disk.device());
	error = fat->mount();
}

Volume::~Volume() {
	delete fat;
}

md_error_t Volume::remount() {
	md_error_t flushError = fat ? fat->flush() : 0;
	delete fat;
	fat = new Fat(disk.device());
	error = fat->mount();
	if (!error) error = flushError;
	return error;
}

uint8_t pattern(uint32_t i, uint32_t seed) {
	return static_cast<uint8_t>((i * 31u + seed * 7u + (i >> 8)) & 0xFF);
}

void fillPattern(uint8_t *buffer, uint32_t length, uint32_t start, uint32_t seed) {
	for (uint32_t i = 0; i < length; i++) buffer[i] = pattern(start + i, seed);
}

bool matchesPattern(const uint8_t *buffer, uint32_t length, uint32_t start, uint32_t seed) {
	for (uint32_t i = 0; i < length; i++) {
		if (buffer[i] != pattern(start + i, seed)) return false;
	}
	return true;
}

md_error_t writeFile(Fat &fat, const char *path, const char *name, uint32_t length, uint32_t seed, uint32_t chunk) {
	File file(fat);
	if (md_error_t error = file.open(path, name, true)) return error;
	if (md_error_t error = file.truncate()) return error;
	static uint8_t buffer[700];
	if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
	for (uint32_t done = 0; done < length; ) {
		uint32_t n = length - done < chunk ? length - done : chunk;
		fillPattern(buffer, n, done, seed);
		if (md_error_t error = file.write(buffer, n)) return error;
		done += n;
	}
	return file.close();
}

md_error_t verifyFile(Fat &fat, const char *path, const char *name, uint32_t length, uint32_t seed, uint32_t chunk) {
	File file(fat);
	if (md_error_t error = file.open(path, name)) return error;
	if (file.length() != length) return TEST_DATA_MISMATCH;
	static uint8_t buffer[700];
	if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
	uint32_t done = 0;
	for (;;) {
		uint32_t n;
		if (md_error_t error = file.read(buffer, chunk, n)) return error;
		if (n == 0) break;
		if (!matchesPattern(buffer, n, done, seed)) return TEST_DATA_MISMATCH;
		done += n;
	}
	return done == length ? 0 : TEST_DATA_MISMATCH;
}

md_error_t countEntries(Fat &fat, const char *path, int &count) {
	count = 0;
	DirectoryListing dl(fat);
	if (md_error_t error = dl.open(path)) return error;
	while (dl.next()) count++;
	return dl.error();
}

int countEntries(Fat &fat, const char *path) {
	int count;
	return countEntries(fat, path, count) ? -1 : count;
}

md_error_t countUsedSlots(Fat &fat, const char *path, int &count) {
	count = 0;
	DirectoryListing dl(fat);
	if (md_error_t error = dl.open(path)) return error;
	Chain chain(fat);
	chain.init(dl.startCluster());
	DirectoryEntry entry;
	for (;;) {
		uint32_t n;
		if (md_error_t error = chain.read(reinterpret_cast<uint8_t*>(&entry), sizeof(entry), n)) return error;
		if (n != sizeof(entry) || entry.DIR_Name[0] == DIR_ENTRY_END) break;
		if (entry.DIR_Name[0] != DIR_ENTRY_DELETED) count++;
	}
	return 0;
}

int countUsedSlots(Fat &fat, const char *path) {
	int count;
	return countUsedSlots(fat, path, count) ? -1 : count;
}
