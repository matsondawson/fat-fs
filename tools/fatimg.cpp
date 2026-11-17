/**
 * fatimg - use the FAT driver on a disk image (or block device) from Linux.
 *
 *   fatimg [-p N] IMAGE create SIZE_MB      create a sparse image and format it FAT32
 *   fatimg [-p N] IMAGE format              format an existing image/device FAT32
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
 * image is treated as a bare FAT volume. Paths may use long names when built
 * with long file name support.
 */

#include "FileSectorDevice.h"

#include "net/mdawson/filesystem/CachedSectorDevice.h"
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/FatFormat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/GlobalError.h"
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
		"usage: fatimg [-p N] IMAGE COMMAND [ARGS]\n"
		"  create SIZE_MB | format | info | ls [DIR] | cat PATH\n"
		"  put LOCALFILE PATH | get PATH LOCALFILE | rm PATH | mkdir PATH\n");
	return 2;
}

static int fail(const char *what) {
	fprintf(stderr, "fatimg: %s failed (error %lu)\n", what, (unsigned long)mdError);
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
static bool flushAll(SectorDevice &volume, FileSectorDevice &device) {
	if (md_error_t error = volume.flush()) {
		mdError = error;
		return false;
	}
	device.sync();
	return true;
}

int main(int argc, char *argv[]) {
	if (argc == 2 && strcmp(argv[1], "--features") == 0) {
#ifdef MFS_LONG_FILE_NAMES
		printf("lfn\n");
#endif
#ifdef MFS_BLOCK_CACHE
		printf("cache\n");
#endif
		return 0;
	}

	int arg = 1;
	int partitionIndex = -1;
	if (arg + 1 < argc && strcmp(argv[arg], "-p") == 0) {
		partitionIndex = atoi(argv[arg + 1]);
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

	Partition partition(storage, partitionIndex < 0 ? Partition::WHOLE_DEVICE : Partition::primary((uint32_t)partitionIndex));
	if (mdError) return fail("reading partition table");
	sector_t volumeSectors = partitionIndex < 0 ? device.getSectorCount() : partition.getSectorCount();

	if (strcmp(command, "format") == 0) {
		formatFat32(partition, volumeSectors);
		if (mdError) return fail("format");
		if (!flushAll(partition, device)) return fail("flush");
		return 0;
	}

	Fat fat(partition);
	if (mdError) return fail("mount");

	if (strcmp(command, "info") == 0) {
		printf("FAT32, %lu clusters of %lu bytes, %lu free\n",
			(unsigned long)fat.getClusterCount(), (unsigned long)fat.getBytesPerCluster(),
			(unsigned long)fat.countFreeClusters());
		return 0;
	}

	if (strcmp(command, "ls") == 0) {
		DirectoryListing dl(fat, remaining > 0 ? argv[arg] : "/");
		if (mdError) return fail("ls");
		char name[LFN_MAX_LENGTH * 3 + 1];
		while (dl.next()) {
			const DirectoryEntry *e = dl.directoryEntry();
			dl.name(name, sizeof(name));
			printf("%s", name);
			if (e->DIR_Attr & ATTR_DIRECTORY) printf("/\n");
			else printf("\t%lu\n", (unsigned long)e->DIR_FileSize);
		}
		return mdError ? fail("ls") : 0;
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
		File file(fat, dir, name);
		if (mdError) return fail("open");
		uint8_t buffer[4096];
		for (;;) {
			uint32_t n = file.read(buffer, sizeof(buffer));
			if (mdError) return fail("read");
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
		File file(fat, dir, name, true);
		if (mdError) return fail("create");
		file.truncate(); // replace existing contents
		if (mdError) return fail("truncate");
		uint8_t buffer[4096];
		size_t n;
		while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
			file.write(buffer, (uint32_t)n);
			if (mdError) return fail("write");
		}
		fclose(in);
		if (!flushAll(partition, device)) return fail("flush");
		return 0;
	}

	if (strcmp(command, "rm") == 0) {
		File file(fat, dir, name);
		if (mdError) return fail("open");
		file.remove();
		if (mdError) return fail("rm");
		if (!flushAll(partition, device)) return fail("flush");
		return 0;
	}

	if (strcmp(command, "mkdir") == 0) {
		makeDirectory(fat, dir, name);
		if (mdError) return fail("mkdir");
		if (!flushAll(partition, device)) return fail("flush");
		return 0;
	}

	return usage();
}
