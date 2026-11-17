#ifndef net_mdawson_error_GlobalError
#define net_mdawson_error_GlobalError

#include "net/mdawson/common/Types.h"

extern md_error_t mdError;

#define mdRaiseError(_value) mdError=_value;return;

/**
 * Error enumerations.
 */
enum {
	/** Device is not present */
	SectorDevice_notPresent = 1000,
	/** Device is not ready */
	SectorDevice_notReady,
	/** Device is read only */
	SectorDevice_notWritable,
	/** If device has changed since last init */
	SectorDevice_changed,
	/** Attempt to read or write sector out of range */
	SectorDevice_sectorOutOfRange,
	/** I/O error reported by the underlying device */
	SectorDevice_ioError,
	/** A memory backed device has no room left to store another sector */
	SectorDevice_storageFull,
};

enum {
	/** Offset is outside the sector */
	Sector_indexOutOfRange = 2000,
};

enum {
	/** Sector parameter exceeds partitions size */
	Partition_sectorOutOfRange = 3000,
	/** The partition table has an invalid format */
	Partition_tableInvalid,
};

enum {
	/** Sector parameter exceeds partitions size */
	Fat_invalidDriveFormat_bs_jmpboot = 4000,
	Fat_invalidDriveFormat_sector_size_unsupported,
	Fat_invalidDriveFormat_sectorsPerCluster,
	Fat_invalidDriveFormat_cluster_exceeds_32k,
	Fat_invalidDriveFormat_reserved_sectors_is_zero,
	Fat_invalidDriveFormat_numfats_must_not_be_zero,
	Fat_invalidDriveFormat_rootEntryCountNotMultipleOfSectorSize,
	Fat_invalidDriveFormat_rootEntryCountNotMultipleOfSectorSize_FatSz16,
	Fat_invalidDriveFormat_rootEntryCountMustNotBeZero,
	Fat_invalidDriveFormat_rootEntryCountMustNotBeZero_FatSz32,
	Fat_invalidDriveFormat_rootClusterIndex,
	Fat_invalidDriveFormat_bootBlockEndSignatureNotAA55,
	/** The FATs and root directory don't fit in the volume's sector count */
	Fat_invalidDriveFormat_volumeTooSmall,
	/** The FAT is too small to hold an entry for every cluster */
	Fat_invalidDriveFormat_fatTooSmall,
	Fat_partitionIsFull,
	Fat_Fat12FormatUnsupported,
	Fat_Fat16FormatUnsupported,
	Fat_CannotAccessBeyondCluster,
	/** A cluster number outside the volume's data region was used */
	Fat_clusterOutOfRange,
	/** Volume is not mounted (construction failed) */
	Fat_notMounted,
	/** Too few sectors for a FAT32 volume with the requested cluster size */
	Fat_formatVolumeTooSmall,
	/** Sectors per cluster must be a power of two from 1 to 64 */
	Fat_formatInvalidClusterSize,
	/** The FAT12/FAT16 root directory has a fixed size, and it has no free entries left */
	Fat_rootDirectoryFull,
	/** Too many sectors for a FAT12/FAT16 volume with the requested cluster size */
	Fat_formatVolumeTooLarge,
};

enum {
	File_fileNotFound = 6000,
	File_seekExceedsFileSize,
	/** Operation on a File whose construction failed */
	File_notOpen,
	/** Name is not a valid 8.3 short name */
	File_invalidName,
	/** Tried to open a directory as a file, or create something that exists */
	File_isDirectory,
	File_alreadyExists,

	DirectoryListing_directoryNotFound = 8000,
	DirectoryListing_invalidPathFormat,
};

enum {
	Chain_seekExceedsChainLength = 7000,
	/** The cluster chain is longer than the volume, i.e. the FAT contains a loop */
	Chain_loopDetected,
};


#endif