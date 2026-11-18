#include "Chain.h"

#include "net/mdawson/filesystem/Error.h"

MFS_NAMESPACE_BEGIN

Chain::Chain(Fat& fat) : m_fat(&fat) {
	init(0);
}

void Chain::init(cluster_t firstCluster) {
	m_firstCluster = firstCluster;
	reset();
}

void Chain::reset() {
	m_currentCluster = m_firstCluster;
	m_run = 0;
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

md_error_t Chain::nextCluster(bool &moved) {
	return advance(false, moved);
}

md_error_t Chain::advance(bool allocate, bool &moved) {
	moved = false;
	if (m_currentCluster == 0) {
		// Empty chain: the "next" cluster is the first one.
		if (!allocate) return 0;
		cluster_t newCluster;
		if (md_error_t error = m_fat->allocateCluster(0, newCluster)) return error;
		m_firstCluster = newCluster;
		m_currentCluster = newCluster;
		m_chainPosition = 0;
		m_clusterOffset = 0;
		moved = true;
		return 0;
	}

	// A chain can never be longer than the volume: if it is, the FAT has a loop.
	if (m_chainPosition / m_fat->getBytesPerCluster() + 1 >= m_fat->getClusterCount()) {
		return Chain_loopDetected;
	}

	cluster_t nextCluster;
	if (m_run > 0) {
		nextCluster = m_currentCluster + 1;
		m_run--;
	} else {
		if (md_error_t error = m_fat->getEntry(m_currentCluster, nextCluster, m_run)) return error;
		if (m_fat->isEndOfClusterChain(nextCluster)) {
			if (!allocate) return 0;
			if (md_error_t error = m_fat->allocateCluster(m_currentCluster, nextCluster)) return error;
		}
	}

	m_chainPosition += currentClusterBytes();
	m_currentCluster = nextCluster;
	m_clusterOffset = 0;
	moved = true;
	return 0;
}

md_error_t Chain::truncate() {
	if (m_currentCluster == 0) return 0;

	if (position() == 0) {
		return freeAll();
	}

	cluster_t next;
	if (md_error_t error = m_fat->getEntry(m_currentCluster, next)) return error;
	if (!m_fat->isEndOfClusterChain(next)) {
		// The clusters after this one are going
		m_run = 0;
		if (md_error_t error = m_fat->setEntry(m_currentCluster, m_fat->getEndOfClusterChain() | 0x7)) return error;
		return m_fat->freeChain(next);
	}
	return 0;
}

md_error_t Chain::freeAll() {
	md_error_t error = 0;
	if (m_firstCluster != 0) {
		error = m_fat->freeChain(m_firstCluster);
	}
	init(0);
	return error;
}

/**
 * Seeks by cluster size blocks through chain.
 */
md_error_t Chain::seek(uint32_t position) {
	// Only rewind when seeking backwards; forward seeks continue from here.
	if (position < m_chainPosition) {
		reset();
	}

	while (position - m_chainPosition > currentClusterBytes() ||
	       (position - m_chainPosition == currentClusterBytes() && m_currentCluster == 0)) {
		bool moved;
		if (md_error_t error = nextCluster(moved)) return error;
		if (!moved) return Chain_seekExceedsChainLength;
	}
	if (m_currentCluster == 0 && position != 0) {
		return Chain_seekExceedsChainLength;
	}
	// position - m_chainPosition == the cluster size is allowed: "end of the current cluster".
	m_clusterOffset = position - m_chainPosition;
	return 0;
}

md_error_t Chain::write(const uint8_t* data, uint32_t bytesToWrite) {
	while (bytesToWrite > 0) {
		if (m_currentCluster == 0 || m_clusterOffset == currentClusterBytes()) {
			bool moved;
			if (md_error_t error = advance(true, moved)) return error;
		}

		uint32_t chunk = currentClusterBytes() - m_clusterOffset;
		if (chunk > bytesToWrite) chunk = bytesToWrite;

		if (md_error_t error = m_fat->write(m_currentCluster, m_clusterOffset, data, chunk)) return error;

		m_clusterOffset += chunk;
		data += chunk;
		bytesToWrite -= chunk;
	}
	return 0;
}

md_error_t Chain::read(uint8_t* data, uint32_t bytesToRead, uint32_t &bytesRead) {
	bytesRead = 0;

	while (bytesToRead > 0) {
		if (m_currentCluster == 0) break;
		if (m_clusterOffset == currentClusterBytes()) {
			bool moved;
			if (md_error_t error = advance(false, moved)) return error;
			if (!moved) break;
		}

		uint32_t chunk = currentClusterBytes() - m_clusterOffset;
		if (chunk > bytesToRead) chunk = bytesToRead;

		if (md_error_t error = m_fat->read(m_currentCluster, m_clusterOffset, data, chunk)) return error;

		m_clusterOffset += chunk;
		data += chunk;
		bytesToRead -= chunk;
		bytesRead += chunk;
	}
	return 0;
}

MFS_NAMESPACE_END
