#include "SdCardTests.h"

#include "net/mdawson/filesystem/CachedSectorDevice.h"
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/Error.h"
#include "net/mdawson/filesystem/Partition.h"

#include "pico/time.h"

#include <stdio.h>
#include <string.h>

using namespace Mfs;

namespace {

int g_failures;

// Passes reads to the card, and writes only when allowed, so the read only run
// can't change the card even if the driver tried to
class WriteGuard : public SectorDevice {
public:
	WriteGuard(SectorDevice &device, bool allowWrites) : m_device(device), m_allowWrites(allowWrites), m_blocked(0) {}
	md_error_t read(uint8_t *data, sector_t sector) override { return m_device.read(data, sector); }
	md_error_t write(const uint8_t *data, sector_t sector) override {
		if (!m_allowWrites) {
			m_blocked++;
			return SectorDevice_notWritable;
		}
		return m_device.write(data, sector);
	}
	uint32_t getBytesPerSector() const override { return m_device.getBytesPerSector(); }
	md_error_t flush() override { return m_device.flush(); }
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
	File file(fat);
	if (file.open("/", name)) {
		return false;
	}
	absolute_time_t start = get_absolute_time();
	h = 2166136261u;
	uint32_t total = 0;
	while (total < limit) {
		uint32_t want = limit - total < sizeof(buffer) ? limit - total : sizeof(buffer);
		uint32_t got;
		if (file.read(buffer, want, got)) {
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

	{
		File file(fat);
		md_error_t error = file.open("/", name, true);
		check(!error && file.isOpen(), "write: create FATTEST.TMP");
		if (error) {
			return;
		}
		error = file.seek(0);
		if (!error) {
			error = file.truncate();
		}
		absolute_time_t start = get_absolute_time();
		for (uint32_t done = 0; done < length && !error;) {
			uint32_t n = length - done < sizeof(buffer) ? length - done : sizeof(buffer);
			for (uint32_t i = 0; i < n; i++) {
				buffer[i] = patternByte(done + i);
			}
			error = file.write(buffer, n);
			done += n;
		}
		if (!error) {
			error = file.flush();
		}
		int64_t us = absolute_time_diff_us(start, get_absolute_time());
		check(!error && file.length() == length, "write: write 64 KB and flush");
		if (error) {
			printf("    error %lu\n", static_cast<unsigned long>(error));
		} else if (us > 0) {
			printf("    %lu bytes in %lld ms, %lu KB/s\n", (unsigned long)length, (long long)(us / 1000),
			       (unsigned long)((uint64_t)length * 1000000 / us / 1024));
		}
	}

	{
		File file(fat);
		bool ok = file.open("/", name) == 0 && file.length() == length;
		for (uint32_t done = 0; ok && done < length;) {
			uint32_t got;
			if (file.read(buffer, sizeof(buffer), got) || got == 0) {
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
		check(file.remove() == 0, "write: delete FATTEST.TMP");
	}

	File gone(fat);
	check(gone.open("/", name) == File_fileNotFound, "write: FATTEST.TMP gone");
}

// Sector hashes from reading one sector at a time, each in its own stream
const uint32_t STREAM_SECTORS = 128;
uint32_t g_reference[STREAM_SECTORS];

uint32_t sectorHash(const uint8_t *data) {
	return hash(2166136261u, data, 512);
}

// Reads sectors start + order[i] and checks each against the reference
bool readInOrder(PicoSdCard &card, sector_t start, const uint32_t *order, uint32_t count) {
	static uint8_t sector[512];
	for (uint32_t i = 0; i < count; i++) {
		if (card.read(sector, start + order[i]) || sectorHash(sector) != g_reference[order[i]]) {
			printf("    sector %lu differs (step %lu)\n", (unsigned long)(start + order[i]), (unsigned long)i);
			return false;
		}
	}
	return true;
}

// Reads count sectors from start in order, spending workUs after each as a
// decoder would, returning KB/s, or 0 on failure
uint32_t rawSpeed(PicoSdCard &card, sector_t start, uint32_t count, uint32_t workUs) {
	static uint8_t sector[512];
	absolute_time_t begin = get_absolute_time();
	for (uint32_t i = 0; i < count; i++) {
		if (card.read(sector, start + i)) {
			return 0;
		}
		busy_wait_us_32(workUs);
	}
	int64_t us = absolute_time_diff_us(begin, get_absolute_time());
	return us > 0 ? (uint32_t)((uint64_t)count * 512 * 1000000 / us / 1024) : 0;
}

// The multiple block read and read ahead give the same data as single reads,
// whatever order sectors are read in
void streamTests(PicoSdCard &card, sector_t start) {
	static uint8_t sector[512];
	static uint32_t order[STREAM_SECTORS];
	bool readAhead = card.isReadAheadOn();
	printf("    read ahead %s\n", readAhead ? "on" : "off (no DMA channels)");

	// Reference: backwards, so each read opens a new stream
	bool ok = true;
	for (uint32_t i = STREAM_SECTORS; ok && i-- > 0;) {
		ok = card.read(sector, start + i) == 0;
		g_reference[i] = sectorHash(sector);
	}
	uint32_t distinct = 0;
	for (uint32_t i = 0; i < STREAM_SECTORS; i++) {
		distinct += i == 0 || g_reference[i] != g_reference[i - 1];
	}
	check(ok, "stream: read sectors one stream each");
	printf("    %lu sectors from %lu, %lu changes of content\n", (unsigned long)STREAM_SECTORS,
	       (unsigned long)start, (unsigned long)distinct);

	for (uint32_t i = 0; i < STREAM_SECTORS; i++) {
		order[i] = i;
	}
	card.resetStats();
	ok = readInOrder(card, start, order, STREAM_SECTORS);
	const PicoSdCard::Stats &stats = card.getStats();
	check(ok && stats.streamStarts == 1, "stream: in order, one stream");
	if (readAhead) {
		check(stats.readAheadHits >= STREAM_SECTORS / 2, "stream: in order, read ahead used");
		printf("    %lu of %lu reads from the read ahead\n", (unsigned long)stats.readAheadHits,
		       (unsigned long)stats.reads);
	}

	// Every other sector: the stream moves past the skipped one
	uint32_t n = 0;
	for (uint32_t i = 0; i < STREAM_SECTORS; i += 2) {
		order[n++] = i;
	}
	check(readInOrder(card, start, order, n), "stream: every other sector");

	// Runs of 3, jumping about, and the same sector twice
	n = 0;
	for (uint32_t run = 0; run < 16; run++) {
		uint32_t first = (run * 37) % (STREAM_SECTORS - 3);
		order[n++] = first;
		order[n++] = first + 1;
		order[n++] = first + 1;
		order[n++] = first + 2;
	}
	check(readInOrder(card, start, order, n), "stream: short runs and repeats");

	// flush() closes the stream, and reading carries on afterwards
	check(card.flush() == 0, "stream: flush closes the stream");
	order[0] = 5;
	order[1] = 6;
	check(readInOrder(card, start, order, 2), "stream: read after flush");

	// Without read ahead
	card.setReadAhead(false);
	for (uint32_t i = 0; i < STREAM_SECTORS; i++) {
		order[i] = i;
	}
	check(readInOrder(card, start, order, STREAM_SECTORS), "stream: in order without read ahead");
	// With 250 us of other work per sector, as a decoder needs, read ahead
	// overlaps the transfer with the work
	for (uint32_t workUs = 0; workUs <= 250; workUs += 250) {
		card.setReadAhead(false);
		uint32_t slow = rawSpeed(card, start, 1024, workUs);
		card.setReadAhead(readAhead);
		uint32_t fast = rawSpeed(card, start, 1024, workUs);
		check(slow && fast, "stream: 512 KB of sectors");
		printf("    512 KB, %lu us work per sector: %lu KB/s without read ahead, %lu KB/s with\n",
		       (unsigned long)workUs, (unsigned long)slow, (unsigned long)fast);
	}
	check(card.flush() == 0, "stream: flush at the end");
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
	Partition partition(guard);
	Partition whole(guard);
	Fat fat(partition);
	Fat wholeFat(whole);
	Fat *mounted = &fat;
	bool onWholeCard = false;
	error = partition.open(Partition::primary(0));
	if (!error) {
		error = fat.mount();
	}
	if (error) {
		onWholeCard = true;
		mounted = &wholeFat;
		error = whole.open(Partition::WHOLE_DEVICE);
		if (!error) {
			error = wholeFat.mount();
		}
	}
	check(!error, "mount FAT32 volume");
	if (error) {
		printf("    error %lu: is the card FAT32?\n", static_cast<unsigned long>(error));
		return g_failures;
	}

	{
		// Sectors well into the data region, likely file contents: the volume's
		// first data sector, from its boot sector, plus 1000
		sector_t volume = onWholeCard ? 0 : partition.getSectorOffset();
		sector_t start = volume;
		if (card.read(sector, volume) == 0) {
			uint32_t reserved = sector[14] | sector[15] << 8;
			uint32_t fats = sector[16];
			uint32_t fatSize = sector[22] | sector[23] << 8;
			if (fatSize == 0) {
				fatSize = sector[36] | sector[37] << 8 | sector[38] << 16 | (uint32_t)sector[39] << 24;
			}
			uint32_t rootSectors = ((sector[17] | sector[18] << 8) * 32 + 511) / 512;
			start = volume + reserved + fats * fatSize + rootSectors + 1000;
		}
		if (start + 2048 > card.getSectorCount()) {
			start = volume;
		}
		streamTests(card, start);
	}

	char firstFile[256] = "";
	uint32_t firstLength = 0;
	{
		DirectoryListing listing(*mounted);
		error = listing.open("/");
		uint32_t entries = 0;
		while (!error && listing.next()) {
			char name[256];
			listing.name(name, sizeof(name));
			const DirectoryEntry &entry = listing.directoryEntry();
			bool directory = (entry.DIR_Attr & ATTR_DIRECTORY) != 0;
			printf("    %s%s  %lu\n", name, directory ? "/" : "", (unsigned long)entry.DIR_FileSize);
			if (!directory && !firstFile[0] && entry.DIR_FileSize > 0) {
				strncpy(firstFile, name, sizeof(firstFile) - 1);
				firstLength = entry.DIR_FileSize;
			}
			entries++;
		}
		if (!error) {
			error = listing.error();
		}
		check(!error, "list root directory");
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
			Partition cachedPartition(cache);
			Fat cachedFat(cachedPartition);
			uint32_t h3 = 0;
			int64_t us3 = 0;
			bool ok3 = cachedPartition.open(onWholeCard ? Partition::WHOLE_DEVICE : Partition::primary(0)) == 0 &&
			           cachedFat.mount() == 0 && readFile(cachedFat, firstFile, limit, h3, us3);
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

	check(mounted->flush() == 0, "flush the volume");
	if (!write) {
		check(guard.blockedWrites() == 0, "read only run tried no writes");
	}
	printf("sd: %d failed\n", g_failures);
	return g_failures;
}
