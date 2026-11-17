#ifndef net_mdawson_filesystems_Sector
#define net_mdawson_filesystems_Sector

#include "net/mdawson/filesystem/SectorDevice.h"

#define SECTOR_SIZE 512

/**
 * A single sector buffer. The buffer is heap allocated (rather than a member array)
 * to keep stack usage low on microcontrollers.
 */
class Sector
{
private:
	sector_t m_sector;
	SectorDevice *m_sectorDevice;

public:
	uint8_t *m_data;

	Sector(SectorDevice &device);
	~Sector(void);
	Sector(const Sector&) = delete;
	Sector& operator=(const Sector&) = delete;

	void set(uint32_t offset, uint8_t value);
	uint8_t get(uint32_t offset);
	md_error_t read(sector_t sector);
	md_error_t write(sector_t sector);
};

#endif
