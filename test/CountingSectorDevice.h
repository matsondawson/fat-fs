#ifndef net_mdawson_test_CountingSectorDevice
#define net_mdawson_test_CountingSectorDevice

#include "net/mdawson/filesystem/SectorDevice.h"

using namespace Mfs;

/**
 * Test helper: wraps a device, counts reads/writes/flushes, remembers the order of
 * recent writes, and can inject failures.
 */
class CountingSectorDevice : public SectorDevice
{
public:
	static const sector_t ANY_SECTOR = (sector_t)-1;
	static const sector_t NO_SECTOR = (sector_t)-2;

	SectorDevice *device;
	uint32_t reads;
	uint32_t writes;
	uint32_t flushes;

	/** Fail reads/writes of this sector (ANY_SECTOR = all, NO_SECTOR = none). */
	sector_t failReadSector;
	sector_t failWriteSector;

	/** The last WRITE_LOG_SIZE sectors written, oldest first. */
	static const uint32_t WRITE_LOG_SIZE = 16;
	sector_t writeLog[WRITE_LOG_SIZE];
	uint32_t writeLogLength;

	explicit CountingSectorDevice(SectorDevice &device);

	void resetCounts();

	virtual md_error_t read(uint8_t* data, sector_t sector);
	virtual md_error_t write(const uint8_t* data, sector_t sector);
	virtual uint32_t getBytesPerSector() const;
	virtual md_error_t flush();
};

#endif
