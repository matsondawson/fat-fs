#include "MemorySectorDevice.h"

#include "net/mdawson/filesystem/GlobalError.h"

#include <string.h>

MFS_NAMESPACE_BEGIN

static const uint32_t BYTES_PER_SECTOR = 512;

MemorySectorDevice::MemorySectorDevice(sector_t sectorCount, uint32_t maxStoredSectors)
	: m_sectorCount(sectorCount), m_maxStored(maxStoredSectors), m_stored(0) {
	// Keep the open addressing hash table at most 50% full.
	m_capacity = 1;
	while (m_capacity < maxStoredSectors * 2) {
		m_capacity <<= 1;
	}
	m_keys = new sector_t[m_capacity];
	m_index = new uint32_t[m_capacity];
	m_data = new uint8_t[(size_t)maxStoredSectors * BYTES_PER_SECTOR];
	clear();
}

MemorySectorDevice::~MemorySectorDevice(void) {
	delete[] m_keys;
	delete[] m_index;
	delete[] m_data;
}

void MemorySectorDevice::clear() {
	for (uint32_t i = 0; i < m_capacity; i++) {
		m_keys[i] = EMPTY_KEY;
	}
	m_stored = 0;
}

static inline uint32_t hashSector(sector_t sector) {
	uint32_t h = sector * 2654435761u;
	return h ^ (h >> 16);
}

/** Slot holding sector, or the empty slot where it would be inserted. */
int32_t MemorySectorDevice::findSlot(sector_t sector) const {
	uint32_t mask = m_capacity - 1;
	for (uint32_t i = hashSector(sector) & mask; ; i = (i + 1) & mask) {
		if (m_keys[i] == sector || m_keys[i] == EMPTY_KEY) {
			return (int32_t)i;
		}
	}
}

md_error_t MemorySectorDevice::read(uint8_t* data, sector_t sector) {
	if (sector >= m_sectorCount) {
		return SectorDevice_sectorOutOfRange;
	}

	int32_t slot = findSlot(sector);
	if (m_keys[slot] == EMPTY_KEY) {
		memset(data, 0, BYTES_PER_SECTOR);
	} else {
		memcpy(data, m_data + (size_t)m_index[slot] * BYTES_PER_SECTOR, BYTES_PER_SECTOR);
	}
	return 0;
}

md_error_t MemorySectorDevice::write(const uint8_t* data, sector_t sector) {
	if (sector >= m_sectorCount) {
		return SectorDevice_sectorOutOfRange;
	}

	int32_t slot = findSlot(sector);
	if (m_keys[slot] == EMPTY_KEY) {
		// Writing zeros to a sector we don't store is a no-op: it already reads as zero.
		bool allZero = true;
		for (uint32_t i = 0; i < BYTES_PER_SECTOR; i++) {
			if (data[i]) { allZero = false; break; }
		}
		if (allZero) {
			return 0;
		}
		if (m_stored >= m_maxStored) {
			return SectorDevice_storageFull;
		}
		m_keys[slot] = sector;
		m_index[slot] = m_stored++;
	}
	memcpy(m_data + (size_t)m_index[slot] * BYTES_PER_SECTOR, data, BYTES_PER_SECTOR);
	return 0;
}

uint32_t MemorySectorDevice::getBytesPerSector() const {
	return BYTES_PER_SECTOR;
}

MFS_NAMESPACE_END
