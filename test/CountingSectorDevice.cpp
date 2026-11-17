#include "CountingSectorDevice.h"

#include "net/mdawson/filesystem/GlobalError.h"

CountingSectorDevice::CountingSectorDevice(SectorDevice &device)
	: device(&device), failReadSector(NO_SECTOR), failWriteSector(NO_SECTOR) {
	resetCounts();
}

void CountingSectorDevice::resetCounts() {
	reads = writes = flushes = 0;
	writeLogLength = 0;
}

md_error_t CountingSectorDevice::read(uint8_t* data, sector_t sector) {
	reads++;
	if (failReadSector == ANY_SECTOR || failReadSector == sector) return SectorDevice_ioError;
	return device->read(data, sector);
}

md_error_t CountingSectorDevice::write(const uint8_t* data, sector_t sector) {
	writes++;
	if (failWriteSector == ANY_SECTOR || failWriteSector == sector) return SectorDevice_ioError;
	if (writeLogLength == WRITE_LOG_SIZE) {
		for (uint32_t i = 1; i < WRITE_LOG_SIZE; i++) writeLog[i - 1] = writeLog[i];
		writeLogLength--;
	}
	writeLog[writeLogLength++] = sector;
	return device->write(data, sector);
}

uint32_t CountingSectorDevice::getBytesPerSector() const {
	return device->getBytesPerSector();
}

md_error_t CountingSectorDevice::flush() {
	flushes++;
	return device->flush();
}
