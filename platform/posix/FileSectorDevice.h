#ifndef net_mdawson_platform_posix_FileSectorDevice
#define net_mdawson_platform_posix_FileSectorDevice

#include "net/mdawson/filesystem/SectorDevice.h"

MFS_NAMESPACE_BEGIN

/**
 * POSIX sector device backed by a disk image file or a block device
 * (e.g. a USB flash drive at /dev/sdX - make sure it is unmounted first).
 */
class FileSectorDevice : public SectorDevice
{
private:
	int m_fd;
	sector_t m_sectorCount;
	bool m_readOnly;

public:
	/**
	 * Open an existing image file or block device.
	 * Check isOpen() afterwards; errno describes any failure.
	 */
	explicit FileSectorDevice(const char *path, bool readOnly = false);
	~FileSectorDevice() override;
	FileSectorDevice(const FileSectorDevice&) = delete;
	FileSectorDevice& operator=(const FileSectorDevice&) = delete;

	/**
	 * Create (or replace) a zero filled, sparse image file of sectorCount sectors.
	 * @return true on success.
	 */
	static bool createImage(const char *path, sector_t sectorCount);

	bool isOpen() const { return m_fd >= 0; }
	sector_t getSectorCount() const { return m_sectorCount; }

	/** Flush writes to the underlying storage. */
	void sync();

	MFS_NODISCARD md_error_t read(uint8_t* data, sector_t sector) override;
	MFS_NODISCARD md_error_t write(const uint8_t* data, sector_t sector) override;
	uint32_t getBytesPerSector() const override;
};

MFS_NAMESPACE_END

#endif
