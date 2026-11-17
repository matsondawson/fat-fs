#ifndef net_mdawson_filesystems_Partition
#define net_mdawson_filesystems_Partition

#include "net/mdawson/filesystem/SectorDevice.h"

/**
 * A view of one partition of a device, using the MBR partition table.
 *
 * The partition argument encodes a path through (possibly nested) partition tables,
 * 3 bits per level with the lowest level first, terminated by the value 7:
 *   Partition::WHOLE_DEVICE        - no partition table (e.g. a "superfloppy" SD card)
 *   Partition::primary(n)          - primary partition n (0-3)
 *
 * After construction check mdError for Partition_tableInvalid etc.
 */
class Partition : public SectorDevice
{
private:
	SectorDevice *m_sectorDevice;
	sector_t m_sectorOffset;
	sector_t m_sectorCount;

	md_error_t decodePartition(uint32_t partition);

public:
	static const uint32_t WHOLE_DEVICE = 7;
	static uint32_t primary(uint32_t index) { return (WHOLE_DEVICE << 3) | (index & 3); }

	Partition(SectorDevice &sectorDevice, uint32_t partition);
	~Partition();

	virtual md_error_t read(uint8_t* data, sector_t sector);
	virtual md_error_t write(const uint8_t* data, sector_t sector);
	virtual uint32_t getBytesPerSector() const;
	virtual md_error_t flush();

	sector_t getSectorOffset() const { return m_sectorOffset; }
	sector_t getSectorCount() const { return m_sectorCount; }
};

#endif
