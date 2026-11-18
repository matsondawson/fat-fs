#ifndef net_mdawson_pico_SdCardTests
#define net_mdawson_pico_SdCardTests

#include "PicoSdCard.h"

/**
 * Checks the FAT driver against a real SD card.
 *
 * Read only unless write is true: it starts the card, reads its boot sector,
 * mounts its FAT32 volume, lists the root directory and reads up to 1 MB of its
 * first file twice, checking both reads agree. With write, it also creates FATTEST.TMP in
 * the root directory, writes and reads it back, then deletes it.
 *
 * @return number of failed checks; a missing card counts as one.
 */
int runSdCardTests(Mfs::PicoSdCard &card, bool write);

#endif
