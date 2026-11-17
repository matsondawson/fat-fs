#include "TestEnv.h"

TestConfig g_config = { 256, 0, false, false, 200 };

Disk::Disk(sector_t sectors, uint32_t maxStored)
	: ram(sectors, maxStored ? maxStored : g_config.ramDiskSectors) {
#ifdef MFS_BLOCK_CACHE
	cache = g_config.cacheSlots ? new CachedSectorDevice(ram, g_config.cacheSlots, g_config.writeThrough, g_config.readAhead) : 0;
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

Volume::Volume(uint8_t sectorsPerCluster, sector_t sectors) : disk(sectors), fat(0) {
	Mfs::formatFat32(disk.device(), sectors, sectorsPerCluster, "TEST");
	if (mdError) return;
	fat = new Fat(disk.device());
}

Volume::~Volume() {
	delete fat;
}

void Volume::remount() {
	if (fat) fat->flush();
	delete fat;
	fat = new Fat(disk.device());
}

uint8_t pattern(uint32_t i, uint32_t seed) {
	return (uint8_t)((i * 31u + seed * 7u + (i >> 8)) & 0xFF);
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

void writeFile(Fat &fat, const char *path, const char *name, uint32_t length, uint32_t seed, uint32_t chunk) {
	Mfs::File file(fat, path, name, true);
	if (mdError) return;
	file.truncate();
	if (mdError) return;
	static uint8_t buffer[700];
	if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
	for (uint32_t done = 0; done < length; ) {
		uint32_t n = length - done < chunk ? length - done : chunk;
		fillPattern(buffer, n, done, seed);
		file.write(buffer, n);
		if (mdError) return;
		done += n;
	}
	file.close();
}

bool verifyFile(Fat &fat, const char *path, const char *name, uint32_t length, uint32_t seed, uint32_t chunk) {
	Mfs::File file(fat, path, name);
	if (mdError || file.length() != length) return false;
	static uint8_t buffer[700];
	if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
	uint32_t done = 0;
	for (;;) {
		uint32_t n = file.read(buffer, chunk);
		if (mdError) return false;
		if (n == 0) break;
		if (!matchesPattern(buffer, n, done, seed)) return false;
		done += n;
	}
	return done == length;
}

int countEntries(Fat &fat, const char *path) {
	Mfs::DirectoryListing dl(fat, path);
	if (mdError) return -1;
	int count = 0;
	while (dl.next()) count++;
	return mdError ? -1 : count;
}

int countUsedSlots(Fat &fat, const char *path) {
	Mfs::DirectoryListing dl(fat, path);
	if (mdError) return -1;
	Chain chain(fat);
	chain.init(dl.startCluster());
	DirectoryEntry entry;
	int used = 0;
	while (chain.read((uint8_t*)&entry, sizeof(entry)) == sizeof(entry)) {
		if (entry.DIR_Name[0] == DIR_ENTRY_END) break;
		if (entry.DIR_Name[0] != DIR_ENTRY_DELETED) used++;
	}
	return mdError ? -1 : used;
}
