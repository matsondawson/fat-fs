#ifndef net_mdawson_filesystems_Sector
#define net_mdawson_filesystems_Sector

#include "net/mdawson/filesystem/SectorDevice.h"

#include <memory>

MFS_NAMESPACE_BEGIN

constexpr uint32_t SECTOR_SIZE = 512;

/**
 * A single sector buffer. The buffer is either the caller's, which must hold
 * SECTOR_SIZE bytes and outlive the Sector, or heap allocated (rather than a
 * member array) to keep stack usage low on microcontrollers.
 */
class Sector
{
private:
	SectorDevice *m_sectorDevice;
	std::unique_ptr<uint8_t[]> m_owned;
	uint8_t *m_data;

public:
	explicit Sector(SectorDevice &device);
	/** Uses buffer, which isn't freed. */
	Sector(SectorDevice &device, uint8_t *buffer);
	Sector(const Sector&) = delete;
	Sector& operator=(const Sector&) = delete;

	uint8_t *data() { return m_data; }
	const uint8_t *data() const { return m_data; }

	MFS_NODISCARD md_error_t read(sector_t sector);
	MFS_NODISCARD md_error_t write(sector_t sector);
};

MFS_NAMESPACE_END

#endif
