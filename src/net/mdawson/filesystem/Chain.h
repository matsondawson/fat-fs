#ifndef net_mdawson_filesystems_Chain
#define net_mdawson_filesystems_Chain

#include "net/mdawson/filesystem/Fat.h"

MFS_NAMESPACE_BEGIN

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
	/**
	 * How many of the clusters after the current one are known to follow it one
	 * after another (from the FAT sector last read for this chain), so moving on
	 * needs no FAT lookup. Lets several files read at once each avoid the FAT.
	 */
	uint32_t m_run;

	/** Move to the next cluster, allocating one if allowed and needed. moved is false at the end. */
	MFS_NODISCARD md_error_t advance(bool allocate, bool &moved);
	/** Size of the current cluster (the FAT12/16 fixed root directory is one large one). */
	uint32_t currentClusterBytes() const;

public:
	explicit Chain(Fat &fat);
	Chain(const Chain&) = delete;
	Chain& operator=(const Chain&) = delete;

	void init(cluster_t firstCluster);

	cluster_t firstCluster() const;
	cluster_t getCluster() const;
	uint32_t clusterOffset() const;

	/** Go to the next cluster. moved is false if there are no more clusters. */
	MFS_NODISCARD md_error_t nextCluster(bool &moved);

	/** Back to the start of the chain. */
	void reset();

	/**
	 * Seek to a byte position. Fails with Chain_seekExceedsChainLength if the chain
	 * is shorter than position.
	 */
	MFS_NODISCARD md_error_t seek(uint32_t position);

	/**
	 * Free every cluster after the current one. If the cursor is at position 0 the
	 * whole chain is freed and the chain becomes empty (firstCluster() == 0).
	 */
	MFS_NODISCARD md_error_t truncate();

	/** Free the whole chain; it becomes empty. */
	MFS_NODISCARD md_error_t freeAll();

	/**
	 * Get current byte position in chain.
	 */
	uint32_t position() const;

	/** Write at the cursor, extending the chain as needed. */
	MFS_NODISCARD md_error_t write(const uint8_t *data, uint32_t bytesToWrite);

	/**
	 * Read at the cursor; stops at the end of the chain. bytesRead is set even
	 * when an error stops the read part way.
	 */
	MFS_NODISCARD md_error_t read(uint8_t *data, uint32_t bytesToRead, uint32_t &bytesRead);
};

MFS_NAMESPACE_END

#endif
