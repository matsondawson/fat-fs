#ifndef net_mdawson_filesystems_Fat
#define net_mdawson_filesystems_Fat

#include "net/mdawson/filesystem/SectorDevice.h"

#include <memory>

MFS_NAMESPACE_BEGIN

using cluster_t = uint32_t;

constexpr uint8_t ATTR_READ_ONLY = 0x01;
constexpr uint8_t ATTR_HIDDEN = 0x02;
constexpr uint8_t ATTR_SYSTEM = 0x04;
constexpr uint8_t ATTR_VOLUME_ID = 0x08;
constexpr uint8_t ATTR_DIRECTORY = 0x10;
constexpr uint8_t ATTR_ARCHIVE = 0x20;
constexpr uint8_t ATTR_LONG_NAME = ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID;
constexpr uint8_t ATTR_LONG_NAME_MASK = ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID | ATTR_DIRECTORY | ATTR_ARCHIVE;

/** DIR_Name[0] value marking a deleted (free) entry. */
constexpr uint8_t DIR_ENTRY_DELETED = 0xE5;
/** DIR_Name[0] value marking a free entry and the end of the directory. */
constexpr uint8_t DIR_ENTRY_END = 0x00;

enum class FatFormat {
	FAT12,
	FAT16,
	FAT32
};

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
 * Construct it on a device, then call mount(). Every operation that can fail
 * returns an md_error_t (0 for success; codes in Error.h) and gives any other
 * result through a reference parameter.
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

	FatFormat m_format;

	/**
	 * The one sector buffer every operation on the volume works in, allocated
	 * when it is mounted. No two uses overlap: each finishes with it before the
	 * next starts.
	 */
	std::unique_ptr<uint8_t[]> m_buffer;

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
	bool isDataCluster(cluster_t cluster) const;
	/** Where an access within a cluster starts. */
	md_error_t locate(cluster_t cluster, uint32_t offset, uint32_t dataSize, sector_t &sector) const;
	/** Read the FAT entry for cluster (which must be in range) through reader. */
	md_error_t readEntry(FatEntryReader &reader, cluster_t cluster, cluster_t &value);
	/** Write a FAT sector to every FAT copy. */
	md_error_t writeFatSector(Sector &sector, sector_t sectorInFirstFat);

public:
	/** A volume on partition; call mount() before using it. */
	explicit Fat(SectorDevice &partition);
	/** Unmount: closes any Files still open on the volume, then flushes the device. */
	~Fat();
	Fat(const Fat&) = delete;
	Fat& operator=(const Fat&) = delete;

	/**
	 * Read the boot sector and check the volume. Does nothing if already mounted.
	 * Fails with a Fat_invalidDriveFormat_ code, Fat_Fat12FormatUnsupported or
	 * Fat_Fat16FormatUnsupported, or a device error.
	 */
	md_error_t mount();
	bool isMounted() const;

	/** Get the FAT entry (next cluster) for a cluster. */
	md_error_t getEntry(cluster_t cluster, cluster_t &next);
	/** Set the FAT entry for a cluster, in every FAT copy. */
	md_error_t setEntry(cluster_t cluster, cluster_t next);

	/**
	 * Allocate a free cluster, mark it end-of-chain, zero its contents and,
	 * if fromCluster is non-zero, link fromCluster to it.
	 */
	md_error_t allocateCluster(cluster_t fromCluster, cluster_t &newCluster);

	/** Free every cluster of the chain starting at firstCluster. */
	md_error_t freeChain(cluster_t firstCluster);

	/** Flush the underlying device (e.g. a write-back cache). */
	md_error_t flush();

	/** Count free clusters by scanning the FAT. */
	md_error_t countFreeClusters(uint32_t &count);

	bool isEndOfClusterChain(cluster_t cluster) const;
	cluster_t getRootDirectoryCluster() const;
	cluster_t getEndOfClusterChain() const;
	uint32_t getBytesPerCluster() const;
	/** Bytes in cluster: getBytesPerCluster(), or the fixed root directory's size for FIXED_ROOT_CLUSTER. */
	uint32_t getClusterBytes(cluster_t cluster) const;
	uint32_t getClusterCount() const;
	FatFormat getFormat() const;

	/**
	 * Write within a single cluster. offset + dataSize must not exceed the cluster size.
	 */
	md_error_t write(cluster_t cluster, uint32_t offset, const uint8_t *data, uint32_t dataSize);

	/**
	 * Read within a single cluster: all dataSize bytes, or an error.
	 * offset + dataSize must not exceed the cluster size.
	 */
	md_error_t read(cluster_t cluster, uint32_t offset, uint8_t *data, uint32_t dataSize);
};

MFS_NAMESPACE_END

#endif
