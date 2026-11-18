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
	if (fat) fat->flush();
	delete fat;
	fat = new Fat(disk.device());
	error = fat->mount();
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

bool verifyFile(Fat &fat, const char *path, const char *name, uint32_t length, uint32_t seed, uint32_t chunk) {
	File file(fat);
	if (file.open(path, name) || file.length() != length) return false;
	static uint8_t buffer[700];
	if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
	uint32_t done = 0;
	for (;;) {
		uint32_t n;
		if (file.read(buffer, chunk, n)) return false;
		if (n == 0) break;
		if (!matchesPattern(buffer, n, done, seed)) return false;
		done += n;
	}
	return done == length;
}

int countEntries(Fat &fat, const char *path) {
	DirectoryListing dl(fat);
	if (dl.open(path)) return -1;
	int count = 0;
	while (dl.next()) count++;
	return dl.error() ? -1 : count;
}

int countUsedSlots(Fat &fat, const char *path) {
	DirectoryListing dl(fat);
	if (dl.open(path)) return -1;
	Chain chain(fat);
	chain.init(dl.startCluster());
	DirectoryEntry entry;
	int used = 0;
	for (;;) {
		uint32_t n;
		if (chain.read(reinterpret_cast<uint8_t*>(&entry), sizeof(entry), n)) return -1;
		if (n != sizeof(entry) || entry.DIR_Name[0] == DIR_ENTRY_END) break;
		if (entry.DIR_Name[0] != DIR_ENTRY_DELETED) used++;
	}
	return used;
}
