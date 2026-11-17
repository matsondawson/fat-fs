/**
 * Host (Linux) runner for the FAT driver tests.
 * Usage: fat_tests [name-filter]
 */
#include "FatTests.h"

int main(int argc, char *argv[]) {
	const char *filter = argc > 1 ? argv[1] : 0;
	return runFatTests(filter, 4096) == 0 ? 0 : 1;
}
