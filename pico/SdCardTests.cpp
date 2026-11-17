#include "SdCardTests.h"

#include "net/mdawson/filesystem/CachedSectorDevice.h"
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/GlobalError.h"
#include "net/mdawson/filesystem/Partition.h"

#include "pico/time.h"

#include <stdio.h>
#include <string.h>

using Mfs::DirectoryListing;
using Mfs::File;

namespace {

int g_failures;

// Passes reads to the card, and writes only when allowed, so the read only run
// can't change the card even if the driver tried to
class WriteGuard : public SectorDevice {
public:
	WriteGuard(SectorDevice &device, bool allowWrites) : m_device(device), m_allowWrites(allowWrites), m_blocked(0) {}
	virtual md_error_t read(uint8_t *data, sector_t sector) { return m_device.read(data, sector); }
	virtual md_error_t write(const uint8_t *data, sector_t sector) {
		if (!m_allowWrites) {
			m_blocked++;
			return SectorDevice_notWritable;
		}
		return m_device.write(data, sector);
	}
	virtual uint32_t getBytesPerSector() const { return m_device.getBytesPerSector(); }
	virtual md_error_t flush() { return m_device.flush(); }
	uint32_t blockedWrites() const { return m_blocked; }

private:
	SectorDevice &m_device;
	bool m_allowWrites;
	uint32_t m_blocked;
};

void check(bool ok, const char *what) {
	printf("[%s] sd/%s\n", ok ? "  OK  " : " FAIL ", what);
	if (!ok) {
		g_failures++;
	}
}

// FNV-1a, to compare two reads of the same file without holding either
uint32_t hash(uint32_t h, const uint8_t *data, uint32_t length) {
	for (uint32_t i = 0; i < length; i++) {
		h = (h ^ data[i]) * 16777619u;
	}
	return h;
}

// Most of a file read, to keep each run short
const uint32_t READ_LIMIT = 1024 * 1024;

// Reads the file, up to limit bytes, returning their hash and how long it took
bool readFile(Fat &fat, const char *name, uint32_t limit, uint32_t &h, int64_t &us) {
	static uint8_t buffer[4096];
	mdError = 0;
	File file(fat, "/", name);
	if (mdError || !file.isOpen()) {
		return false;
	}
	absolute_time_t start = get_absolute_time();
	h = 2166136261u;
	uint32_t total = 0;
	while (total < limit) {
		uint32_t want = limit - total < sizeof(buffer) ? limit - total : sizeof(buffer);
		uint32_t got = file.read(buffer, want);
		if (mdError) {
			return false;
		}
		if (got == 0) {
			break;
		}
		h = hash(h, buffer, got);
		total += got;
	}
	us = absolute_time_diff_us(start, get_absolute_time());
	return total == limit;
}

// Pattern for the write test: depends on the position, so misplaced data shows
uint8_t patternByte(uint32_t i) {
	return (uint8_t)(i * 7 + (i >> 9));
}

void writeTest(Fat &fat) {
	const char *name = "FATTEST.TMP";
	const uint32_t length = 64 * 1024 + 123;
	static uint8_t buffer[1024];

	mdError = 0;
	{
		File file(fat, "/", name, true);
		check(!mdError && file.isOpen(), "write: create FATTEST.TMP");
		if (mdError) {
			return;
		}
		file.seek(0);
		file.truncate();
		absolute_time_t start = get_absolute_time();
		for (uint32_t done = 0; done < length && !mdError;) {
			uint32_t n = length - done < sizeof(buffer) ? length - done : sizeof(buffer);
			for (uint32_t i = 0; i < n; i++) {
				buffer[i] = patternByte(done + i);
			}
			file.write(buffer, n);
			done += n;
		}
		file.flush();
		int64_t us = absolute_time_diff_us(start, get_absolute_time());
		check(!mdError && file.length() == length, "write: write 64 KB and flush");
		if (!mdError && us > 0) {
			printf("    %lu bytes in %lld ms, %lu KB/s\n", (unsigned long)length, (long long)(us / 1000),
			       (unsigned long)((uint64_t)length * 1000000 / us / 1024));
		}
	}

	mdError = 0;
	{
		File file(fat, "/", name);
		bool ok = !mdError && file.isOpen() && file.length() == length;
		for (uint32_t done = 0; ok && done < length;) {
			uint32_t got = file.read(buffer, sizeof(buffer));
			if (mdError || got == 0) {
				ok = false;
				break;
			}
			for (uint32_t i = 0; i < got; i++) {
				if (buffer[i] != patternByte(done + i)) {
					ok = false;
					break;
				}
			}
			done += got;
		}
		check(ok, "write: read back matches");
		mdError = 0;
		file.remove();
		check(!mdError, "write: delete FATTEST.TMP");
	}

	mdError = 0;
	File gone(fat, "/", name);
	check(mdError == File_fileNotFound, "write: FATTEST.TMP gone");
	mdError = 0;
}

} // namespace

int runSdCardTests(PicoSdCard &card, bool write) {
	g_failures = 0;
	printf("\n---- SD card (%s) ----\n", write ? "read and write" : "read only");

	md_error_t error = card.begin();
	if (error) {
		printf("[ FAIL ] sd/start: %s (error %lu)\n", card.lastError(), (unsigned long)error);
		return 1;
	}
	printf("[  OK  ] sd/start: %s, %lu sectors (%lu MB), SPI at %lu Hz\n",
	       card.isHighCapacity() ? "SDHC/SDXC" : "SDSC", (unsigned long)card.getSectorCount(),
	       (unsigned long)(card.getSectorCount() / 2048), (unsigned long)card.getBaudRate());

	static uint8_t sector[512];
	error = card.read(sector, 0);
	check(!error && sector[510] == 0x55 && sector[511] == 0xAA, "read sector 0 (boot signature)");

	error = card.read(sector, card.getSectorCount());
	check(error == SectorDevice_sectorOutOfRange, "read past the end is refused");

	// A partition table, as cards come formatted, or a volume across the whole card
	WriteGuard guard(card, write);
	mdError = 0;
	Partition partition(guard, Partition::primary(0));
	Partition whole(guard, Partition::WHOLE_DEVICE);
	mdError = 0;
	Fat fat(partition);
	Fat *mounted = &fat;
	Fat *fallback = 0;
	if (mdError) {
		mdError = 0;
		fallback = new Fat(whole);
		mounted = fallback;
	}
	check(!mdError, "mount FAT32 volume");
	if (mdError) {
		printf("    mdError %lu: is the card FAT32?\n", (unsigned long)mdError);
		delete fallback;
		mdError = 0;
		return g_failures;
	}

	char firstFile[256] = "";
	uint32_t firstLength = 0;
	{
		mdError = 0;
		DirectoryListing listing(*mounted, "/");
		uint32_t entries = 0;
		while (!mdError && listing.next()) {
			char name[256];
			listing.name(name, sizeof(name));
			DirectoryEntry *entry = listing.directoryEntry();
			bool directory = (entry->DIR_Attr & ATTR_DIRECTORY) != 0;
			printf("    %s%s  %lu\n", name, directory ? "/" : "", (unsigned long)entry->DIR_FileSize);
			if (!directory && !firstFile[0] && entry->DIR_FileSize > 0) {
				strncpy(firstFile, name, sizeof(firstFile) - 1);
				firstLength = entry->DIR_FileSize;
			}
			entries++;
		}
		check(!mdError, "list root directory");
		printf("    %lu entries\n", (unsigned long)entries);
	}

	if (firstFile[0]) {
		uint32_t h1 = 0, h2 = 0;
		int64_t us1 = 0, us2 = 0;
		uint32_t limit = firstLength < READ_LIMIT ? firstLength : READ_LIMIT;
		bool ok1 = readFile(*mounted, firstFile, limit, h1, us1);
		bool ok2 = ok1 && readFile(*mounted, firstFile, limit, h2, us2);
		check(ok1 && ok2 && h1 == h2, "read first file twice, same data");
		if (ok1 && us1 > 0) {
			printf("    %s: first %lu bytes in %lld ms, %lu KB/s\n", firstFile, (unsigned long)limit,
			       (long long)(us1 / 1000), (unsigned long)((uint64_t)limit * 1000000 / us1 / 1024));
		}
#ifdef MFS_BLOCK_CACHE
		// The same, through a cache reading ahead
		{
			CachedSectorDevice cache(guard, 4, false, true);
			Partition cachedPartition(cache, fallback ? Partition::WHOLE_DEVICE : Partition::primary(0));
			mdError = 0;
			Fat cachedFat(cachedPartition);
			uint32_t h3 = 0;
			int64_t us3 = 0;
			bool ok3 = !mdError && readFile(cachedFat, firstFile, limit, h3, us3);
			check(ok1 && ok3 && h3 == h1, "read first file through a read ahead cache, same data");
			if (ok3 && us3 > 0) {
				const CachedSectorDevice::Stats &stats = cache.getStats();
				printf("    through the cache: %lld ms, %lu KB/s; %lu device reads, %lu read ahead, %lu read ahead hits\n",
				       (long long)(us3 / 1000), (unsigned long)((uint64_t)limit * 1000000 / us3 / 1024),
				       (unsigned long)stats.deviceReads, (unsigned long)stats.readAheads,
				       (unsigned long)stats.readAheadHits);
			}
		}
#endif
	} else {
		printf("    no file in the root directory to read\n");
	}

	if (write) {
		writeTest(*mounted);
	}

	mounted->flush();
	delete fallback;
	mdError = 0;
	if (!write) {
		check(guard.blockedWrites() == 0, "read only run tried no writes");
	}
	printf("sd: %d failed\n", g_failures);
	return g_failures;
}
