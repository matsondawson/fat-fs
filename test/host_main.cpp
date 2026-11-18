/**
 * Host (Linux) runner for the FAT driver tests.
 * Usage: fat_tests [name-filter]
 */
#include "FatTests.h"

#include "net/mdawson/filesystem/Error.h"
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/FatFormat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/MemorySectorDevice.h"

#include <stdio.h>
#include <thread>

/**
 * The driver keeps no global state, so threads that each use their own volume
 * don't affect each other. Each thread makes a volume, fails to open a missing
 * file and writes a file, many times over, while the others do the same.
 */
static void volumeWorker(int id, bool *ok) {
	*ok = true;
	Mfs::MemorySectorDevice ram(70000, 512);
	Mfs::Fat fat(ram);
	if (Mfs::formatFat32(ram, 70000, 1) || fat.mount()) {
		*ok = false;
		return;
	}
	char name[13];
	snprintf(name, sizeof(name), "T%d.BIN", id);
	for (int i = 0; i < 200 && *ok; i++) {
		Mfs::File missing(fat);
		if (missing.open("/", "MISSING.TXT") != Mfs::File_fileNotFound) {
			*ok = false;
		}
		Mfs::File file(fat);
		uint8_t data[64] = {static_cast<uint8_t>(id), static_cast<uint8_t>(i)};
		if (file.open("/", name, true) || file.write(data, sizeof(data)) || file.close()) {
			*ok = false;
		}
	}
}

static int runThreadTests() {
	const int THREADS = 4;
	bool ok[THREADS];
	std::thread threads[THREADS];
	for (int i = 0; i < THREADS; i++) {
		threads[i] = std::thread(volumeWorker, i, &ok[i]);
	}
	for (int i = 0; i < THREADS; i++) {
		threads[i].join();
	}
	int failed = 0;
	for (int i = 0; i < THREADS; i++) {
		failed += ok[i] ? 0 : 1;
	}
	printf("[%s] threads/independent_volumes\n", failed ? " FAIL " : "  OK  ");
	return failed;
}

int main(int argc, char *argv[]) {
	const char *filter = argc > 1 ? argv[1] : nullptr;
	int failed = runFatTests(filter, 4096);
	if (!filter || !*filter) {
		failed += runThreadTests();
	}
	return failed == 0 ? 0 : 1;
}
