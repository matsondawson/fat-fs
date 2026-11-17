/**
 * Pico test harness: runs the FAT driver test suite on the microcontroller,
 * against RAM disks, then checks the driver on the SD card.
 *
 * Output goes to USB serial (`pico/pico.sh monitor`). The suite re-runs every
 * 10 seconds so you can attach a terminal at any time. The on-board LED
 * (non-W boards) blinks slowly when all tests pass and quickly when any fail.
 *
 * The SD card's wiring comes from pico/CMakeLists.txt (FAT_PICO_SD_*), and is
 * passed to PicoSdCard at run time; the library itself has no fixed pins.
 */

#include "FatTests.h"
#include "PicoSdCard.h"
#include "SdCardTests.h"

#include "pico/stdlib.h"

#include <stdio.h>

// Maximum non-zero sectors held by each RAM disk: 128 * 512 bytes = 64KB.
// Fits the RP2040's 264KB of RAM with plenty to spare.
static const uint32_t RAM_DISK_SECTORS = 128;

static void blink(int times, int periodMs) {
#ifdef PICO_DEFAULT_LED_PIN
	for (int i = 0; i < times; i++) {
		gpio_put(PICO_DEFAULT_LED_PIN, 1);
		sleep_ms(periodMs / 2);
		gpio_put(PICO_DEFAULT_LED_PIN, 0);
		sleep_ms(periodMs / 2);
	}
#else
	sleep_ms(times * periodMs);
#endif
}

int main() {
	stdio_init_all();

#ifdef PICO_DEFAULT_LED_PIN
	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

	// Give a USB serial terminal a moment to connect.
	for (int i = 0; i < 30 && !stdio_usb_connected(); i++) {
		sleep_ms(100);
	}

	PicoSdCard card(FAT_PICO_SD_SPI ? spi1 : spi0, FAT_PICO_SD_SCK, FAT_PICO_SD_MOSI, FAT_PICO_SD_MISO,
	                FAT_PICO_SD_CS, FAT_PICO_SD_HZ);
	card.setCardDetect(FAT_PICO_SD_DETECT);

	for (uint32_t run = 1; ; run++) {
		printf("\n==== FAT driver tests on Pico, run %lu ====\n", (unsigned long)run);
		absolute_time_t start = get_absolute_time();
		int failures = runFatTests(0, RAM_DISK_SECTORS);
		failures += runSdCardTests(card, FAT_PICO_SD_WRITE_TEST);
		int64_t elapsedMs = absolute_time_diff_us(start, get_absolute_time()) / 1000;
		printf("%s in %lld ms\n", failures ? "FAILED" : "PASSED", (long long)elapsedMs);

		if (failures) blink(50, 200);   // fast blink ~10s
		else blink(10, 1000);           // slow blink ~10s
	}
}
