#include "Chain.h"

#include "net/mdawson/filesystem/GlobalError.h"

Chain::Chain(Fat& fat) : m_fat(&fat) {
	init(0);
}

void Chain::init(const cluster_t firstCluster) {
	m_firstCluster = firstCluster;
	reset();
}

void Chain::reset() {
	m_currentCluster = m_firstCluster;
	m_chainPosition = 0;
	m_clusterOffset = 0;
}

cluster_t Chain::firstCluster() const {
	return m_firstCluster;
}

cluster_t Chain::getCluster() const {
	return m_currentCluster;
}

uint32_t Chain::clusterOffset() const {
	return m_clusterOffset;
}

uint32_t Chain::currentClusterBytes() const {
	return m_currentCluster ? m_fat->getClusterBytes(m_currentCluster) : m_fat->getBytesPerCluster();
}

uint32_t Chain::position() const {
	return m_chainPosition + m_clusterOffset;
}

/**
 * Seek to the next cluster in the chain.
 *
 * @return true if the chain has a next cluster, else returns false.
 */
bool Chain::nextCluster() {
	return advance(false);
}

bool Chain::advance(bool allocate) {
	if (m_currentCluster == 0) {
		// Empty chain: the "next" cluster is the first one.
		if (!allocate) return false;
		cluster_t newCluster = m_fat->allocateCluster(0);
		if (mdError) return false;
		m_firstCluster = newCluster;
		m_currentCluster = newCluster;
		m_chainPosition = 0;
		m_clusterOffset = 0;
		return true;
	}

	// A chain can never be longer than the volume: if it is, the FAT has a loop.
	if (m_chainPosition / m_fat->getBytesPerCluster() + 1 >= m_fat->getClusterCount()) {
		mdError = Chain_loopDetected;
		return false;
	}

	cluster_t nextCluster = m_fat->getEntry(m_currentCluster);
	if (mdError) return false;
	if (m_fat->isEndOfClusterChain(nextCluster)) {
		if (!allocate) return false;
		nextCluster = m_fat->allocateCluster(m_currentCluster);
		if (mdError) return false;
	}

	m_chainPosition += currentClusterBytes();
	m_currentCluster = nextCluster;
	m_clusterOffset = 0;
	return true;
}

void Chain::truncate() {
	if (m_currentCluster == 0) return;

	if (position() == 0) {
		freeAll();
		return;
	}

	cluster_t next = m_fat->getEntry(m_currentCluster);
	if (mdError) return;
	if (!m_fat->isEndOfClusterChain(next)) {
		m_fat->setEntry(m_currentCluster, m_fat->getEndOfClusterChain() | 0x7);
		if (mdError) return;
		m_fat->freeChain(next);
	}
}

void Chain::freeAll() {
	if (m_firstCluster != 0) {
		m_fat->freeChain(m_firstCluster);
	}
	init(0);
}

/**
 * Seeks by cluster size blocks through chain.
 */
void Chain::seek(uint32_t position) {
	// Only rewind when seeking backwards; forward seeks continue from here.
	if (position < m_chainPosition) {
		reset();
	}

	while (position - m_chainPosition > currentClusterBytes() ||
	       (position - m_chainPosition == currentClusterBytes() && m_currentCluster == 0)) {
		if (!nextCluster()) {
			if (!mdError) mdError = Chain_seekExceedsChainLength;
			return;
		}
	}
	if (m_currentCluster == 0 && position != 0) {
		mdError = Chain_seekExceedsChainLength;
		return;
	}
	// position - m_chainPosition == the cluster size is allowed: "end of the current cluster".
	m_clusterOffset = position - m_chainPosition;
}

void Chain::write(const uint8_t* data, uint32_t bytesToWrite) {
	while (bytesToWrite > 0) {
		if (m_currentCluster == 0 || m_clusterOffset == currentClusterBytes()) {
			if (!advance(true)) return;
		}

		uint32_t chunk = currentClusterBytes() - m_clusterOffset;
		if (chunk > bytesToWrite) chunk = bytesToWrite;

		m_fat->write(m_currentCluster, m_clusterOffset, data, chunk);
		if (mdError) return;

		m_clusterOffset += chunk;
		data += chunk;
		bytesToWrite -= chunk;
	}
}

uint32_t Chain::read(uint8_t* data, uint32_t bytesToRead) {
	uint32_t bytesRead = 0;

	while (bytesToRead > 0) {
		if (m_currentCluster == 0) break;
		if (m_clusterOffset == currentClusterBytes()) {
			if (!advance(false)) break;
		}

		uint32_t chunk = currentClusterBytes() - m_clusterOffset;
		if (chunk > bytesToRead) chunk = bytesToRead;

		m_fat->read(m_currentCluster, m_clusterOffset, data, chunk);
		if (mdError) break;

		m_clusterOffset += chunk;
		data += chunk;
		bytesToRead -= chunk;
		bytesRead += chunk;
	}
	return bytesRead;
}
