// Formats a RAM disk, writes a file and reads it back using only the public API.
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/FatFormat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/GlobalError.h"
#include "net/mdawson/filesystem/MemorySectorDevice.h"

#include <stdio.h>
#include <string.h>

// Names the driver also uses, which must not clash with it
class Fat {};
class File {};
static int mdError = 0;

int main() {
	Mfs::MemorySectorDevice ram(70000, 64);
	Mfs::formatFat32(ram, 70000, 1);
	Mfs::Fat fat(ram);
	if (Mfs::mdError) {
		printf("mount failed: %lu\n", (unsigned long)Mfs::mdError);
		return 1;
	}
	{
		Mfs::File file(fat, "/", "HELLO.TXT", true);
		file.write((const uint8_t *)"hello", 5);
	}
	Mfs::File file(fat, "/", "HELLO.TXT");
	char buffer[8] = {0};
	uint32_t got = file.read((uint8_t *)buffer, sizeof(buffer));
	if (Mfs::mdError || got != 5 || strcmp(buffer, "hello") != 0) {
		printf("read back failed: %lu\n", (unsigned long)Mfs::mdError);
		return 1;
	}
	(void)sizeof(Fat);
	(void)sizeof(File);
	printf("ok\n");
	return mdError;
}
