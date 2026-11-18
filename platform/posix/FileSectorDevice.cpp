#include "FileSectorDevice.h"

#include "net/mdawson/filesystem/GlobalError.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#endif

MFS_NAMESPACE_BEGIN

static const uint32_t BYTES_PER_SECTOR = 512;

FileSectorDevice::FileSectorDevice(const char *path, bool readOnly)
	: m_fd(-1), m_sectorCount(0), m_readOnly(readOnly) {
	m_fd = ::open(path, readOnly ? O_RDONLY : O_RDWR);
	if (m_fd < 0) return;

	struct stat st;
	if (fstat(m_fd, &st) != 0) {
		::close(m_fd);
		m_fd = -1;
		return;
	}

	uint64_t bytes = (uint64_t)st.st_size;
#if defined(__linux__) && defined(BLKGETSIZE64)
	if (S_ISBLK(st.st_mode)) {
		uint64_t blockDeviceBytes = 0;
		if (ioctl(m_fd, BLKGETSIZE64, &blockDeviceBytes) == 0) {
			bytes = blockDeviceBytes;
		}
	}
#endif
	uint64_t sectors = bytes / BYTES_PER_SECTOR;
	// sector_t is 32 bits: devices larger than 2TB are clamped.
	m_sectorCount = sectors > 0xFFFFFFFEull ? 0xFFFFFFFEu : (sector_t)sectors;
}

FileSectorDevice::~FileSectorDevice() {
	if (m_fd >= 0) {
		::close(m_fd);
	}
}

bool FileSectorDevice::createImage(const char *path, sector_t sectorCount) {
	int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return false;
	bool ok = ftruncate(fd, (off_t)sectorCount * BYTES_PER_SECTOR) == 0;
	::close(fd);
	return ok;
}

void FileSectorDevice::sync() {
	if (m_fd >= 0) {
		fsync(m_fd);
	}
}

md_error_t FileSectorDevice::read(uint8_t* data, sector_t sector) {
	if (m_fd < 0) return SectorDevice_notPresent;
	if (sector >= m_sectorCount) return SectorDevice_sectorOutOfRange;

	off_t offset = (off_t)sector * BYTES_PER_SECTOR;
	uint32_t done = 0;
	while (done < BYTES_PER_SECTOR) {
		ssize_t n = pread(m_fd, data + done, BYTES_PER_SECTOR - done, offset + done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return SectorDevice_ioError;
		done += (uint32_t)n;
	}
	return 0;
}

md_error_t FileSectorDevice::write(const uint8_t* data, sector_t sector) {
	if (m_fd < 0) return SectorDevice_notPresent;
	if (m_readOnly) return SectorDevice_notWritable;
	if (sector >= m_sectorCount) return SectorDevice_sectorOutOfRange;

	off_t offset = (off_t)sector * BYTES_PER_SECTOR;
	uint32_t done = 0;
	while (done < BYTES_PER_SECTOR) {
		ssize_t n = pwrite(m_fd, data + done, BYTES_PER_SECTOR - done, offset + done);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return SectorDevice_ioError;
		done += (uint32_t)n;
	}
	return 0;
}

uint32_t FileSectorDevice::getBytesPerSector() const {
	return BYTES_PER_SECTOR;
}

MFS_NAMESPACE_END
