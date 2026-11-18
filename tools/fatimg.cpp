/**
 * fatimg - use the FAT driver on a disk image (or block device) from Linux.
 *
 *   fatimg [-p N] [-t TYPE] IMAGE create SIZE_MB   create a sparse image and format it
 *   fatimg [-p N] [-t TYPE] IMAGE format           format an existing image/device
 *   fatimg [-p N] IMAGE info
 *   fatimg [-p N] IMAGE ls [DIR]
 *   fatimg [-p N] IMAGE cat PATH
 *   fatimg [-p N] IMAGE put LOCALFILE PATH  (replaces PATH if it exists)
 *   fatimg [-p N] IMAGE get PATH LOCALFILE
 *   fatimg [-p N] IMAGE rm PATH
 *   fatimg [-p N] IMAGE mkdir PATH
 *
 *   fatimg --features                       print compiled in features
 *
 * -p N uses primary partition N of an MBR partitioned image; without it the
 * image is treated as a bare FAT volume. -t 12, 16 or 32 picks the FAT type to
 * format, default 32; 12 and 16 need a build with FAT12/16 support. Paths may use long names when built
 * with long file name support.
 */

#include "FileSectorDevice.h"

#include "net/mdawson/filesystem/CachedSectorDevice.h"
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/FatFormat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/Error.h"
#include "net/mdawson/filesystem/Partition.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

using namespace Mfs;

static void localFatTime(uint16_t *date, uint16_t *time_) {
	time_t now = time(0);
	struct tm tm;
	localtime_r(&now, &tm);
	*date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
	*time_ = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
}

static int usage() {
	fprintf(stderr,
		"usage: fatimg [-p N] [-t 12|16|32] IMAGE COMMAND [ARGS]\n"
		"  create SIZE_MB | format | info | ls [DIR] | cat PATH\n"
		"  put LOCALFILE PATH | get PATH LOCALFILE | rm PATH | mkdir PATH\n");
	return 2;
}

static int fail(const char *what, md_error_t error) {
	fprintf(stderr, "fatimg: %s failed (error %lu)\n", what, static_cast<unsigned long>(error));
	return 1;
}

/** Split "/a/b/c.txt" into directory "/a/b" and name "c.txt". */
static bool splitPath(const char *path, char *dir, size_t dirSize, const char **name) {
	const char *slash = strrchr(path, '/');
	if (!slash) {
		snprintf(dir, dirSize, "/");
		*name = path;
	} else {
		size_t len = (size_t)(slash - path);
		if (len + 1 > dirSize) return false;
		memcpy(dir, path, len);
		dir[len] = 0;
		if (len == 0) snprintf(dir, dirSize, "/");
		*name = slash + 1;
	}
	return **name != 0;
}

/** Write back the cache (if any) and sync the image file. */
static md_error_t flushAll(SectorDevice &volume, FileSectorDevice &device) {
	if (md_error_t error = volume.flush()) return error;
	device.sync();
	return 0;
}

int main(int argc, char *argv[]) {
	if (argc == 2 && strcmp(argv[1], "--features") == 0) {
#ifdef MFS_LONG_FILE_NAMES
		printf("lfn\n");
#endif
#ifdef MFS_BLOCK_CACHE
		printf("cache\n");
#endif
#ifdef MFS_FAT12_16
		printf("fat12_16\n");
#endif
		return 0;
	}

	int arg = 1;
	int partitionIndex = -1;
	FatFormat formatType = FatFormat::FAT32;
	while (arg + 1 < argc && argv[arg][0] == '-') {
		if (strcmp(argv[arg], "-p") == 0) {
			partitionIndex = atoi(argv[arg + 1]);
		} else if (strcmp(argv[arg], "-t") == 0) {
			int bits = atoi(argv[arg + 1]);
			if (bits == 12) formatType = FatFormat::FAT12;
			else if (bits == 16) formatType = FatFormat::FAT16;
			else if (bits == 32) formatType = FatFormat::FAT32;
			else return usage();
		} else {
			return usage();
		}
		arg += 2;
	}
	if (argc - arg < 2) return usage();
	const char *image = argv[arg++];
	const char *command = argv[arg++];
	int remaining = argc - arg;

	getFatTime = localFatTime;

	if (strcmp(command, "create") == 0) {
		if (remaining != 1) return usage();
		unsigned long megabytes = strtoul(argv[arg], 0, 10);
		if (!FileSectorDevice::createImage(image, (sector_t)(megabytes * 2048))) {
			fprintf(stderr, "fatimg: cannot create %s: %s\n", image, strerror(errno));
			return 1;
		}
		command = "format";
	}

	bool readOnly = strcmp(command, "info") == 0 || strcmp(command, "ls") == 0 ||
	                strcmp(command, "cat") == 0 || strcmp(command, "get") == 0;
	FileSectorDevice device(image, readOnly);
	if (!device.isOpen()) {
		fprintf(stderr, "fatimg: cannot open %s: %s\n", image, strerror(errno));
		return 1;
	}

#ifdef MFS_BLOCK_CACHE
	// A modest cache makes image access much faster; it is flushed on exit.
	CachedSectorDevice cache(device, 64);
	SectorDevice &storage = cache;
#else
	SectorDevice &storage = device;
#endif

	Partition partition(storage);
	md_error_t error = partition.open(partitionIndex < 0 ? Partition::WHOLE_DEVICE : Partition::primary((uint32_t)partitionIndex));
	if (error) return fail("reading partition table", error);
	sector_t volumeSectors = partitionIndex < 0 ? device.getSectorCount() : partition.getSectorCount();

	if (strcmp(command, "format") == 0) {
		if ((error = formatFat(partition, volumeSectors, formatType))) return fail("format", error);
		if ((error = flushAll(partition, device))) return fail("flush", error);
		return 0;
	}

	Fat fat(partition);
	if ((error = fat.mount())) return fail("mount", error);

	if (strcmp(command, "info") == 0) {
		const char *type = fat.getFormat() == FatFormat::FAT12 ? "FAT12" : fat.getFormat() == FatFormat::FAT16 ? "FAT16" : "FAT32";
		uint32_t freeClusters;
		if ((error = fat.countFreeClusters(freeClusters))) return fail("info", error);
		printf("%s, %lu clusters of %lu bytes, %lu free\n", type,
			(unsigned long)fat.getClusterCount(), (unsigned long)fat.getBytesPerCluster(),
			(unsigned long)freeClusters);
		return 0;
	}

	if (strcmp(command, "ls") == 0) {
		DirectoryListing dl(fat);
		if ((error = dl.open(remaining > 0 ? argv[arg] : "/"))) return fail("ls", error);
		char name[LFN_MAX_LENGTH * 3 + 1];
		while (dl.next()) {
			const DirectoryEntry &e = dl.directoryEntry();
			dl.name(name, sizeof(name));
			printf("%s", name);
			if (e.DIR_Attr & ATTR_DIRECTORY) printf("/\n");
			else printf("\t%lu\n", (unsigned long)e.DIR_FileSize);
		}
		return dl.error() ? fail("ls", dl.error()) : 0;
	}

	if (remaining < 1) return usage();
	char dir[256];
	const char *name;
	const char *path = strcmp(command, "put") == 0 ? (remaining == 2 ? argv[arg + 1] : "") : argv[arg];
	if (!splitPath(path, dir, sizeof(dir), &name)) return usage();

	if (strcmp(command, "cat") == 0 || strcmp(command, "get") == 0) {
		FILE *out = stdout;
		if (strcmp(command, "get") == 0) {
			if (remaining != 2) return usage();
			out = fopen(argv[arg + 1], "wb");
			if (!out) {
				fprintf(stderr, "fatimg: cannot write %s: %s\n", argv[arg + 1], strerror(errno));
				return 1;
			}
		}
		File file(fat);
		if ((error = file.open(dir, name))) return fail("open", error);
		uint8_t buffer[4096];
		for (;;) {
			uint32_t n;
			if ((error = file.read(buffer, sizeof(buffer), n))) return fail("read", error);
			if (n == 0) break;
			fwrite(buffer, 1, n, out);
		}
		if (out != stdout) fclose(out);
		return 0;
	}

	if (strcmp(command, "put") == 0) {
		if (remaining != 2) return usage();
		FILE *in = fopen(argv[arg], "rb");
		if (!in) {
			fprintf(stderr, "fatimg: cannot read %s: %s\n", argv[arg], strerror(errno));
			return 1;
		}
		File file(fat);
		if ((error = file.open(dir, name, true))) return fail("create", error);
		if ((error = file.truncate())) return fail("truncate", error); // replace existing contents
		uint8_t buffer[4096];
		size_t n;
		while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
			if ((error = file.write(buffer, (uint32_t)n))) return fail("write", error);
		}
		fclose(in);
		if ((error = file.close())) return fail("close", error);
		if ((error = flushAll(partition, device))) return fail("flush", error);
		return 0;
	}

	if (strcmp(command, "rm") == 0) {
		File file(fat);
		if ((error = file.open(dir, name))) return fail("open", error);
		if ((error = file.remove())) return fail("rm", error);
		if ((error = flushAll(partition, device))) return fail("flush", error);
		return 0;
	}

	if (strcmp(command, "mkdir") == 0) {
		if ((error = makeDirectory(fat, dir, name))) return fail("mkdir", error);
		if ((error = flushAll(partition, device))) return fail("flush", error);
		return 0;
	}

	return usage();
}
