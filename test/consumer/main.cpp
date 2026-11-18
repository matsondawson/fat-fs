// Formats a RAM disk, writes a file and reads it back using only the public API.
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/FatFormat.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/Error.h"
#include "net/mdawson/filesystem/MemorySectorDevice.h"

#include <stdio.h>
#include <string.h>

// Names the driver also uses, which must not clash with it
class Fat {};
class File {};
static int SECTOR_SIZE = 0;

int main() {
	Mfs::MemorySectorDevice ram(70000, 64);
	Mfs::md_error_t error = Mfs::formatFat32(ram, 70000, 1);
	if (error) {
		printf("format failed: %lu\n", static_cast<unsigned long>(error));
		return 1;
	}
	Mfs::Fat fat(ram);
	if ((error = fat.mount())) {
		printf("mount failed: %lu\n", static_cast<unsigned long>(error));
		return 1;
	}
	{
		Mfs::File file(fat);
		if ((error = file.open("/", "HELLO.TXT", true)) ||
		    (error = file.write(reinterpret_cast<const uint8_t *>("hello"), 5)) ||
		    (error = file.close())) {
			printf("write failed: %lu\n", static_cast<unsigned long>(error));
			return 1;
		}
	}
	Mfs::File file(fat);
	char buffer[8] = {0};
	uint32_t got = 0;
	if ((error = file.open("/", "HELLO.TXT")) == 0)
		error = file.read(reinterpret_cast<uint8_t *>(buffer), sizeof(buffer), got);
	if (error || got != 5 || strcmp(buffer, "hello") != 0) {
		printf("read back failed: %lu\n", static_cast<unsigned long>(error));
		return 1;
	}
	(void)sizeof(Fat);
	(void)sizeof(File);
	printf("ok\n");
	return SECTOR_SIZE;
}
