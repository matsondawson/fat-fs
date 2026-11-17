#ifndef net_mdawson_filesystems_Chain
#define net_mdawson_filesystems_Chain

#include "net/mdawson/filesystem/Fat.h"

/**
 * A cursor over a cluster chain (the data of a file or directory).
 *
 * The cursor is (current cluster, byte offset within that cluster). The offset may
 * equal the cluster size, meaning "at the end of this cluster"; the next read or
 * write moves on to (or allocates) the following cluster.
 *
 * A chain whose first cluster is 0 is empty; writing to it allocates its first
 * cluster (see firstCluster()).
 */
class Chain
{
private:
	Fat *m_fat;

	/** Byte position, within the chain, of the start of the current cluster. */
	uint32_t m_chainPosition;
	/** Byte offset within the current cluster. */
	uint32_t m_clusterOffset;
	cluster_t m_firstCluster;
	cluster_t m_currentCluster;

	/** Move to the next cluster, allocating one if allowed and needed. */
	bool advance(bool allocate);

public:
	Chain(Fat &fat);
	Chain(const Chain&) = delete;
	Chain& operator=(const Chain&) = delete;

	void init(const cluster_t firstCluster);

	cluster_t firstCluster() const;
	cluster_t getCluster() const;
	uint32_t clusterOffset() const;

	/**
	 * Go to next cluster.  Returns false if there are no more clusters.
	 */
	bool nextCluster();

	/** Back to the start of the chain. */
	void reset();

	/**
	 * Seek to a byte position. Fails with Chain_seekExceedsChainLength if the chain
	 * is shorter than position.
	 */
	void seek(uint32_t position);

	/**
	 * Free every cluster after the current one. If the cursor is at position 0 the
	 * whole chain is freed and the chain becomes empty (firstCluster() == 0).
	 */
	void truncate();

	/** Free the whole chain; it becomes empty. */
	void freeAll();

	/**
	 * Get current byte position in chain.
	 */
	uint32_t position() const;

	/** Write at the cursor, extending the chain as needed. */
	void write(const uint8_t *data, uint32_t bytesToWrite);

	/** Read at the cursor; stops at the end of the chain. @return bytes read. */
	uint32_t read(uint8_t *data, uint32_t bytesToRead);
};

#endif
