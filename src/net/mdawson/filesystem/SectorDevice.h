#ifndef net_mdawson_filesystems_SectorDevice
#define net_mdawson_filesystems_SectorDevice

#include "net/mdawson/common/Types.h"

MFS_NAMESPACE_BEGIN

using sector_t = uint32_t;

/**
 * Common class that all block devices must implement (SD card, flash, RAM disk,
 * disk image file, ...). This is the only part of the driver that touches hardware.
 *
 * TODO: we need some way of detecting a notifying that a device has changed.
 */
class SectorDevice
{
public:
	virtual ~SectorDevice() = default;

	/*******************************************
	 * Interfaces that need implementing
	 *******************************************/

	/**
	 * Read a sector from the device into data (getBytesPerSector() bytes).
	 * If the sector exceeds the devices maximum sector count an error should be returned
	 *
	 * @return The errorCode.
	 * @errors SectorDevice_notPresent, SectorDevice_notReady, SectorDevice_changed, SectorDevice_sectorOutOfRange
	 */
	MFS_NODISCARD virtual md_error_t read(uint8_t* data, sector_t sector) = 0;

	/**
	 * Write a sector the the device.
	 * If the sector exceeds the devices maximum sector count as error should be returned
	 *
	 * @return The errorCode.
	 * @errors SectorDevice_notPresent, SectorDevice_notReady, SectorDevice_changed, SectorDevice_sectorOutOfRange,
	 *         SectorDevice_notWritable
	 */
	MFS_NODISCARD virtual md_error_t write(const uint8_t* data, sector_t sector) = 0;

	/** Sector size in bytes. The FAT layer currently requires 512. */
	virtual uint32_t getBytesPerSector() const = 0;

	/**
	 * Make sure every completed write is stored on the media (e.g. write back a
	 * cache). Optional: the default does nothing.
	 */
	MFS_NODISCARD virtual md_error_t flush() { return 0; }
};

MFS_NAMESPACE_END

#endif
