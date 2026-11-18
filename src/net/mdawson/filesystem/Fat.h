#ifndef net_mdawson_filesystems_Fat
#define net_mdawson_filesystems_Fat

#include "net/mdawson/filesystem/SectorDevice.h"

MFS_NAMESPACE_BEGIN

typedef uint32_t cluster_t;

const int ATTR_READ_ONLY = 0x01;
const int ATTR_HIDDEN = 0x02;
const int ATTR_SYSTEM = 0x04;
const int ATTR_VOLUME_ID = 0x08;
const int ATTR_DIRECTORY = 0x10;
const int ATTR_ARCHIVE = 0x20;
const int ATTR_LONG_NAME = (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID);
const int ATTR_LONG_NAME_MASK = (ATTR_READ_ONLY |ATTR_HIDDEN |ATTR_SYSTEM |ATTR_VOLUME_ID |ATTR_DIRECTORY |ATTR_ARCHIVE);

/** DIR_Name[0] value marking a deleted (free) entry. */
const int DIR_ENTRY_DELETED = 0xE5;
/** DIR_Name[0] value marking a free entry and the end of the directory. */
const int DIR_ENTRY_END = 0x00;

typedef enum {
	Format_FAT12,
	Format_FAT16,
	Format_FAT32
} FatFormat_e;

class FatEntryReader;
class Sector;
class File;

/**
 * A mounted FAT volume.
 *
 * FAT32 volumes are always supported. FAT12 and FAT16 volumes need MFS_FAT12_16
 * (CMake option FAT_FAT12_16); without it, mounting one fails with
 * Fat_Fat12FormatUnsupported or Fat_Fat16FormatUnsupported.
 *
 * On FAT12/FAT16 the root directory is a fixed run of sectors rather than a
 * cluster chain. It is addressed as cluster FIXED_ROOT_CLUSTER, one "cluster" of
 * getClusterBytes(FIXED_ROOT_CLUSTER) bytes that can't grow: adding an entry when
 * it is full fails with Fat_rootDirectoryFull.
 *
 * After construction check mdError (or isMounted()) before using the volume.
 *
 * A file can only be open through one File at a time. Destroying the Fat closes
 * any Files still open on it, so they never write to an unmounted volume.
 */
class Fat
{
public:
	/** The cluster number standing for the FAT12/FAT16 fixed root directory. */
	static const cluster_t FIXED_ROOT_CLUSTER = 1;

private:
	SectorDevice *m_partition;
	uint32_t m_bytesPerSector;
	uint32_t m_bytesPerCluster;
	uint32_t m_sectorsPerCluster;
	/**
	 * Count of sectors used by the root directory for Fat12/Fat16
	 */
	uint32_t m_rootDirectorySectorCount;
	uint32_t m_sectorsPerFatTable;
	uint32_t m_numFats;
	uint32_t m_sectorCount;
	uint32_t m_dataSectorCount;
	sector_t m_firstDataSector;
	uint32_t m_clusterCount;
	uint32_t m_reservedSectors;
	sector_t m_infoSector;
	cluster_t m_lastFreeCluster;
	cluster_t m_endOfChainCluster;
	bool m_mounted;
	bool m_fsInfoInvalidated;

	FatFormat_e m_format;

	/**
	 * The one sector buffer every operation on the volume works in, allocated
	 * when it is mounted. No two uses overlap: each finishes with it before the
	 * next starts.
	 */
	uint8_t *m_buffer;

	/** Files open on this volume, linked through File::m_nextOpen. */
	File *m_openFiles;
	friend class File;

	/** The root directory's first cluster, or FIXED_ROOT_CLUSTER on FAT12/FAT16. */
	cluster_t m_rootCluster;
	/** First sector of the FAT12/FAT16 fixed root directory. */
	sector_t m_rootSector;

	md_error_t init();
	sector_t clusterToSector(cluster_t cluster) const;
	void invalidateFsInfo();
	bool isFixedRoot(cluster_t cluster) const;
	/** Where an access within a cluster starts. Sets mdError and returns false if it's out of range. */
	bool locate(cluster_t cluster, uint32_t offset, uint32_t dataSize, sector_t *sector);
	/** Read the FAT entry for cluster (which must be in range) through reader. */
	md_error_t readEntry(FatEntryReader &reader, cluster_t cluster, cluster_t *value);
	/** Write a FAT sector to every FAT copy. */
	md_error_t writeFatSector(Sector &sector, sector_t sectorInFirstFat);

public:
	Fat(SectorDevice &partition);
	/** Unmount: closes any Files still open on the volume, then flushes the device. */
	~Fat();
	Fat(const Fat&) = delete;
	Fat& operator=(const Fat&) = delete;

	bool isMounted() const;

	/** Get the FAT entry (next cluster) for a cluster. */
	cluster_t getEntry(cluster_t cluster);
	/** Set the FAT entry for a cluster, in every FAT copy. */
	void setEntry(cluster_t cluster, cluster_t nextCluster);

	/**
	 * Allocate a free cluster, mark it end-of-chain, zero its contents and,
	 * if fromCluster is non-zero, link fromCluster to it.
	 *
	 * @return the new cluster, or 0 on failure (mdError is set).
	 */
	cluster_t allocateCluster(cluster_t fromCluster);

	/** Free every cluster of the chain starting at firstCluster. */
	void freeChain(cluster_t firstCluster);

	/** Flush the underlying device (e.g. a write-back cache). Sets mdError on failure. */
	void flush();

	/** Count free clusters by scanning the FAT. */
	uint32_t countFreeClusters();

	bool isEndOfClusterChain(cluster_t cluster) const;
	cluster_t getRootDirectoryCluster() const;
	cluster_t getEndOfClusterChain() const;
	uint32_t getBytesPerCluster() const;
	/** Bytes in cluster: getBytesPerCluster(), or the fixed root directory's size for FIXED_ROOT_CLUSTER. */
	uint32_t getClusterBytes(cluster_t cluster) const;
	uint32_t getClusterCount() const;
	FatFormat_e getFormat() const;

	/**
	 * Write within a single cluster. offset + dataSize must not exceed the cluster size.
	 */
	void write(cluster_t cluster, uint32_t offset, const uint8_t *data, uint32_t dataSize);

	/**
	 * Read within a single cluster. offset + dataSize must not exceed the cluster size.
	 * @return number of bytes read.
	 */
	uint32_t read(cluster_t cluster, uint32_t offset, uint8_t *data, uint32_t dataSize);
};

MFS_NAMESPACE_END

#endif
