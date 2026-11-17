#ifndef net_mdawson_filesystems_FatFormat
#define net_mdawson_filesystems_FatFormat

#include "net/mdawson/filesystem/SectorDevice.h"

namespace Mfs {
	/**
	 * Format sectors [0, sectorCount) of device as a FAT32 volume
	 * (32 reserved sectors, 2 FATs, root directory in cluster 2).
	 *
	 * @param sectorsPerCluster 1..64 (power of two), or 0 to choose the
	 *        Microsoft recommended size for the volume size.
	 * @param label Volume label (up to 11 characters), or 0 for "NO NAME".
	 * @param volumeId Volume serial number.
	 *
	 * Sets mdError on failure (e.g. Fat_formatVolumeTooSmall: FAT32 needs at least
	 * 65525 clusters, about 33MB with 512 byte clusters).
	 */
	void formatFat32(SectorDevice &device, sector_t sectorCount, uint8_t sectorsPerCluster = 0,
	                 const char *label = 0, uint32_t volumeId = 0x4D465321);
}

#endif
