#include "CachedSectorDevice.h"

#ifdef MFS_BLOCK_CACHE

#include <string.h>

MFS_NAMESPACE_BEGIN

static const uint32_t BYTES_PER_SECTOR = 512;

CachedSectorDevice::CachedSectorDevice(SectorDevice &device, uint32_t slots, bool writeThrough, bool readAhead)
	: m_device(&device), m_slotCount(slots ? slots : 1), m_writeThrough(writeThrough), m_readAhead(readAhead), m_tick(0) {
	m_sectors = new sector_t[m_slotCount];
	m_lastUsed = new uint32_t[m_slotCount];
	m_dirty = new bool[m_slotCount];
	m_readAheadSlot = new bool[m_slotCount];
	m_data = new uint8_t[(size_t)m_slotCount * BYTES_PER_SECTOR];
	invalidate();
	resetStats();
}

CachedSectorDevice::~CachedSectorDevice() {
	flush();
	delete[] m_sectors;
	delete[] m_lastUsed;
	delete[] m_dirty;
	delete[] m_readAheadSlot;
	delete[] m_data;
}

void CachedSectorDevice::invalidate() {
	for (uint32_t i = 0; i < m_slotCount; i++) {
		m_sectors[i] = INVALID_SECTOR;
		m_lastUsed[i] = 0;
		m_dirty[i] = false;
		m_readAheadSlot[i] = false;
	}
}

void CachedSectorDevice::resetStats() {
	memset(&m_stats, 0, sizeof(m_stats));
}

uint32_t CachedSectorDevice::getBytesPerSector() const {
	return BYTES_PER_SECTOR;
}

uint32_t CachedSectorDevice::getDirtyCount() const {
	uint32_t count = 0;
	for (uint32_t i = 0; i < m_slotCount; i++) {
		if (m_sectors[i] != INVALID_SECTOR && m_dirty[i]) count++;
	}
	return count;
}

uint8_t *CachedSectorDevice::slotData(uint32_t slot) const {
	return m_data + (size_t)slot * BYTES_PER_SECTOR;
}

int32_t CachedSectorDevice::findSlot(sector_t sector) const {
	for (uint32_t i = 0; i < m_slotCount; i++) {
		if (m_sectors[i] == sector) return (int32_t)i;
	}
	return -1;
}

md_error_t CachedSectorDevice::writeBack(uint32_t slot) {
	if (m_sectors[slot] == INVALID_SECTOR || !m_dirty[slot]) return 0;
	m_stats.deviceWrites++;
	md_error_t error = m_device->write(slotData(slot), m_sectors[slot]);
	if (error) return error; // stays dirty
	m_dirty[slot] = false;
	return 0;
}

int32_t CachedSectorDevice::claimSlot(md_error_t *error) {
	*error = 0;
	uint32_t victim = 0;
	for (uint32_t i = 0; i < m_slotCount; i++) {
		if (m_sectors[i] == INVALID_SECTOR) {
			return (int32_t)i;
		}
		if (m_lastUsed[i] < m_lastUsed[victim]) victim = i;
	}
	*error = writeBack(victim);
	if (*error) return -1;
	m_sectors[victim] = INVALID_SECTOR;
	m_readAheadSlot[victim] = false;
	return (int32_t)victim;
}

void CachedSectorDevice::readAhead(sector_t sector) {
	if (sector == INVALID_SECTOR || findSlot(sector) >= 0) return;
	md_error_t error;
	int32_t slot = claimSlot(&error);
	if (slot < 0) return; // a dirty victim couldn't be written back; it stays cached
	m_stats.deviceReads++;
	m_stats.readAheads++;
	if (m_device->read(slotData((uint32_t)slot), sector)) return; // slot stays unused
	m_sectors[slot] = sector;
	m_dirty[slot] = false;
	m_readAheadSlot[slot] = true;
	m_lastUsed[slot] = ++m_tick;
}

md_error_t CachedSectorDevice::read(uint8_t* data, sector_t sector) {
	int32_t slot = findSlot(sector);
	// Read the next sector too: after a miss, or when this one was itself read ahead
	bool next = false;
	if (slot >= 0) {
		m_stats.hits++;
		if (m_readAheadSlot[slot]) {
			m_stats.readAheadHits++;
			m_readAheadSlot[slot] = false;
			next = m_readAhead;
		}
	} else {
		m_stats.misses++;
		md_error_t error;
		slot = claimSlot(&error);
		if (slot < 0) return error;
		m_stats.deviceReads++;
		error = m_device->read(slotData((uint32_t)slot), sector);
		if (error) return error; // slot stays unused
		m_sectors[slot] = sector;
		m_dirty[slot] = false;
		m_readAheadSlot[slot] = false;
		next = m_readAhead;
	}
	m_lastUsed[slot] = ++m_tick;
	// Copied out first: with one slot, reading ahead reuses this one
	memcpy(data, slotData((uint32_t)slot), BYTES_PER_SECTOR);
	if (next) readAhead(sector + 1);
	return 0;
}

md_error_t CachedSectorDevice::write(const uint8_t* data, sector_t sector) {
	int32_t slot = findSlot(sector);
	if (slot >= 0) {
		m_stats.hits++;
	} else {
		m_stats.misses++;
	}

	if (m_writeThrough) {
		m_stats.deviceWrites++;
		md_error_t error = m_device->write(data, sector);
		if (error) {
			// The device may now hold anything: don't keep a possibly stale copy.
			if (slot >= 0) m_sectors[slot] = INVALID_SECTOR;
			return error;
		}
	}

	if (slot < 0) {
		// A whole sector is written, so there is no need to read it first.
		md_error_t error;
		slot = claimSlot(&error);
		if (slot < 0) return error;
		m_sectors[slot] = sector;
	}
	memcpy(slotData((uint32_t)slot), data, BYTES_PER_SECTOR);
	m_dirty[slot] = !m_writeThrough;
	m_readAheadSlot[slot] = false;
	m_lastUsed[slot] = ++m_tick;
	return 0;
}

md_error_t CachedSectorDevice::flush() {
	md_error_t firstError = 0;
	// Write back in ascending sector order (friendlier to flash). A sector that fails
	// stays dirty for a later retry; the others are still written.
	sector_t previous = 0;
	bool first = true;
	for (;;) {
		int32_t next = -1;
		for (uint32_t i = 0; i < m_slotCount; i++) {
			if (m_sectors[i] == INVALID_SECTOR || !m_dirty[i]) continue;
			if (!first && m_sectors[i] <= previous) continue;
			if (next < 0 || m_sectors[i] < m_sectors[next]) next = (int32_t)i;
		}
		if (next < 0) break;
		previous = m_sectors[next];
		first = false;
		md_error_t error = writeBack((uint32_t)next);
		if (error && !firstError) firstError = error;
	}
	if (firstError) return firstError;
	return m_device->flush();
}

MFS_NAMESPACE_END

#endif // MFS_BLOCK_CACHE
