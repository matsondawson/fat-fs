#include "Sector.h"

MFS_NAMESPACE_BEGIN

Sector::Sector(SectorDevice &sectorDevice)
	: m_sectorDevice(&sectorDevice), m_owned(new uint8_t[SECTOR_SIZE]), m_data(m_owned.get()) {
}

Sector::Sector(SectorDevice &sectorDevice, uint8_t *buffer)
	: m_sectorDevice(&sectorDevice), m_data(buffer) {
}

md_error_t Sector::read(sector_t sector) {
	return m_sectorDevice->read(m_data, sector);
}

md_error_t Sector::write(sector_t sector) {
	return m_sectorDevice->write(m_data, sector);
}

MFS_NAMESPACE_END
