/**
 * Host (Linux) runner for the FAT driver tests.
 * Usage: fat_tests [name-filter]
 */
#include "FatTests.h"

#ifdef MFS_THREAD_LOCAL_ERROR
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/FatFormat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/GlobalError.h"
#include "net/mdawson/filesystem/MemorySectorDevice.h"

#include <stdio.h>
#include <thread>

/**
 * With a thread local mdError, threads that each use their own volume see only
 * their own errors. Each thread makes a volume, fails to open a missing file and
 * writes a file, many times over, while the others do the same.
 */
static void volumeWorker(int id, bool *ok) {
	*ok = true;
	Mfs::MemorySectorDevice ram(70000, 512);
	Mfs::mdError = 0;
	Mfs::formatFat32(ram, 70000, 1);
	Mfs::Fat fat(ram);
	if (Mfs::mdError) {
		*ok = false;
		return;
	}
	char name[13];
	snprintf(name, sizeof(name), "T%d.BIN", id);
	for (int i = 0; i < 200 && *ok; i++) {
		{
			Mfs::File missing(fat, "/", "MISSING.TXT");
			if (Mfs::mdError != Mfs::File_fileNotFound) {
				*ok = false;
			}
		}
		Mfs::mdError = 0;
		Mfs::File file(fat, "/", name, true);
		uint8_t data[64] = {(uint8_t)id, (uint8_t)i};
		file.write(data, sizeof(data));
		file.close();
		if (Mfs::mdError) {
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
	Mfs::mdError = 0;
	printf("[%s] threads/thread_local_errors\n", failed ? " FAIL " : "  OK  ");
	return failed;
}
#endif

int main(int argc, char *argv[]) {
	const char *filter = argc > 1 ? argv[1] : 0;
	int failed = runFatTests(filter, 4096);
#ifdef MFS_THREAD_LOCAL_ERROR
	if (!filter || !*filter) {
		failed += runThreadTests();
	}
#endif
	return failed == 0 ? 0 : 1;
}
