#include "Sector.h"

#include "net/mdawson/filesystem/GlobalError.h"

MFS_NAMESPACE_BEGIN

Sector::Sector(SectorDevice &sectorDevice) {
	m_sector = (sector_t)-1;
	m_data = new uint8_t[SECTOR_SIZE];

	m_sectorDevice = &sectorDevice;
}


Sector::~Sector(void) {
	delete[] m_data;
}

void Sector::set(uint32_t offset, uint8_t value) {
	if (offset >= SECTOR_SIZE) {
		mdError = Sector_indexOutOfRange;
		return;
	}
	m_data[offset] = value;
}

uint8_t Sector::get(uint32_t offset) {
	if (offset >= SECTOR_SIZE) {
		mdError = Sector_indexOutOfRange;
		return 0;
	}
	return m_data[offset];
}

md_error_t Sector::read(sector_t sector) {
	m_sector = sector;
	return m_sectorDevice->read(m_data, m_sector);
}

md_error_t Sector::write(sector_t sector) {
	m_sector = sector;
	return m_sectorDevice->write(m_data, m_sector);
}

MFS_NAMESPACE_END
