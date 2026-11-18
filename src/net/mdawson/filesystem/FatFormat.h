#ifndef net_mdawson_filesystems_FatFormat
#define net_mdawson_filesystems_FatFormat

#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/SectorDevice.h"

MFS_NAMESPACE_BEGIN

	/**
	 * Format sectors [0, sectorCount) of device as a FAT32 volume
	 * (32 reserved sectors, 2 FATs, root directory in cluster 2).
	 *
	 * @param sectorsPerCluster 1..64 (power of two), or 0 to choose the
	 *        Microsoft recommended size for the volume size.
	 * @param label Volume label (up to 11 characters), or 0 for "NO NAME".
	 * @param volumeId Volume serial number.
	 *
	 * Fails with Fat_formatVolumeTooSmall (FAT32 needs at least 65525 clusters,
	 * about 33MB with 512 byte clusters), Fat_formatInvalidClusterSize or a device error.
	 */
	MFS_NODISCARD md_error_t formatFat32(SectorDevice &device, sector_t sectorCount, uint8_t sectorsPerCluster = 0,
	                       const char *label = nullptr, uint32_t volumeId = 0x4D465321);

	/**
	 * Format sectors [0, sectorCount) of device as a FAT12, FAT16 or FAT32 volume.
	 * FAT32 is formatFat32(). FAT12 and FAT16 need MFS_FAT12_16 (otherwise it fails
	 * with Fat_Fat12FormatUnsupported / Fat_Fat16FormatUnsupported) and use 1 reserved
	 * sector, 2 FATs and a 512 entry root directory.
	 *
	 * @param sectorsPerCluster 1..64 (power of two), or 0 for the smallest size that
	 *        keeps the cluster count within the type's limit (FAT12: under 4085,
	 *        FAT16: 4085 to 65524).
	 *
	 * Fails with Fat_formatVolumeTooSmall or Fat_formatVolumeTooLarge when the
	 * cluster count doesn't suit the type.
	 */
	MFS_NODISCARD md_error_t formatFat(SectorDevice &device, sector_t sectorCount, FatFormat format, uint8_t sectorsPerCluster = 0,
	                     const char *label = nullptr, uint32_t volumeId = 0x4D465321);

MFS_NAMESPACE_END

#endif
