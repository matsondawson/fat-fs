#ifndef net_mdawson_filesystems_MemorySectorDevice
#define net_mdawson_filesystems_MemorySectorDevice

#include "net/mdawson/filesystem/SectorDevice.h"

#include <memory>

MFS_NAMESPACE_BEGIN

/**
 * Sparse RAM backed sector device.
 *
 * The device reports sectorCount sectors, but only sectors that have ever been
 * written with non-zero data consume memory (up to maxStoredSectors of them);
 * every other sector reads back as zeros. This lets a full size FAT32 volume
 * (which needs at least 65525 clusters) live in a few kilobytes of RAM, which is
 * what makes it possible to run the test suite on a microcontroller.
 */
class MemorySectorDevice : public SectorDevice
{
private:
	sector_t m_sectorCount;
	uint32_t m_capacity;     // hash table slots (power of two)
	uint32_t m_maxStored;
	uint32_t m_stored;
	std::unique_ptr<sector_t[]> m_keys;  // hash slot -> sector number, EMPTY_KEY when unused
	std::unique_ptr<uint32_t[]> m_index; // hash slot -> index into m_data
	std::unique_ptr<uint8_t[]> m_data;   // maxStoredSectors * 512 bytes, filled in insertion order

	static constexpr sector_t EMPTY_KEY = static_cast<sector_t>(-1);

	static uint32_t capacityFor(uint32_t maxStoredSectors);

	int32_t findSlot(sector_t sector) const;

public:
	/**
	 * @param sectorCount Size of the device in 512 byte sectors.
	 * @param maxStoredSectors Maximum number of distinct non-zero sectors kept in RAM.
	 */
	MemorySectorDevice(sector_t sectorCount, uint32_t maxStoredSectors);
	MemorySectorDevice(const MemorySectorDevice&) = delete;
	MemorySectorDevice& operator=(const MemorySectorDevice&) = delete;

	/** Forget all data, every sector reads as zero again. */
	void clear();

	sector_t getSectorCount() const { return m_sectorCount; }
	uint32_t getStoredSectorCount() const { return m_stored; }

	/**
	 * @see SectorDevice
	 */
	MFS_NODISCARD md_error_t read(uint8_t* data, sector_t sector) override;
	MFS_NODISCARD md_error_t write(const uint8_t* data, sector_t sector) override;
	uint32_t getBytesPerSector() const override;
};

MFS_NAMESPACE_END

#endif
