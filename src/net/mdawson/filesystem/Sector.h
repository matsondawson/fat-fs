#ifndef net_mdawson_filesystems_Sector
#define net_mdawson_filesystems_Sector

#include "net/mdawson/filesystem/SectorDevice.h"

MFS_NAMESPACE_BEGIN

const int SECTOR_SIZE = 512;

/**
 * A single sector buffer. The buffer is either the caller's, which must hold
 * SECTOR_SIZE bytes and outlive the Sector, or heap allocated (rather than a
 * member array) to keep stack usage low on microcontrollers.
 */
class Sector
{
private:
	sector_t m_sector;
	SectorDevice *m_sectorDevice;
	bool m_ownsData;

public:
	uint8_t *m_data;

	Sector(SectorDevice &device);
	/** Uses buffer, which isn't freed. */
	Sector(SectorDevice &device, uint8_t *buffer);
	~Sector(void);
	Sector(const Sector&) = delete;
	Sector& operator=(const Sector&) = delete;

	void set(uint32_t offset, uint8_t value);
	uint8_t get(uint32_t offset);
	md_error_t read(sector_t sector);
	md_error_t write(sector_t sector);
};

MFS_NAMESPACE_END

#endif
