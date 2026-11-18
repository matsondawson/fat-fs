#include "Fat.h"

#include "net/mdawson/filesystem/Endian.h"
#include "net/mdawson/filesystem/Error.h"
#include "net/mdawson/filesystem/FatStructures.h"
#include "net/mdawson/filesystem/File.h"
#include "net/mdawson/filesystem/Sector.h"

#include <stddef.h>
#include <string.h>

MFS_NAMESPACE_BEGIN

constexpr cluster_t FREE_CLUSTER = 0;
constexpr uint32_t FAT32_ENTRY_MASK = 0x0FFFFFFFu;
constexpr sector_t NO_SECTOR = static_cast<sector_t>(-1);

/** Reads FAT entries through one sector buffer, reading a sector only when it changes. */
class FatEntryReader {
public:
	FatEntryReader(SectorDevice &device, uint8_t *buffer) : m_sector(device, buffer), m_loaded(NO_SECTOR) {}

	md_error_t load(sector_t sector) {
		if (sector == m_loaded) return 0;
		m_loaded = NO_SECTOR;
		md_error_t error = m_sector.read(sector);
		if (!error) m_loaded = sector;
		return error;
	}

	const uint8_t *data() const { return m_sector.data(); }

private:
	Sector m_sector;
	sector_t m_loaded;
};

Fat::Fat(SectorDevice &partition)
	: m_partition(&partition), m_bytesPerSector(0), m_bytesPerCluster(0), m_sectorsPerCluster(0),
	  m_rootDirectorySectorCount(0), m_sectorsPerFatTable(0), m_numFats(0), m_sectorCount(0),
	  m_dataSectorCount(0), m_firstDataSector(0), m_clusterCount(0), m_reservedSectors(0), m_infoSector(0),
	  m_lastFreeCluster(0), m_endOfChainCluster(0), m_mounted(false), m_fsInfoInvalidated(false),
	  m_format(FatFormat::FAT32), m_openFiles(nullptr), m_rootCluster(0), m_rootSector(0) {
}

Fat::~Fat() {
	if (m_mounted) {
		// Errors can't be reported from a destructor. close() unlinks each File.
		while (m_openFiles) {
			(void)m_openFiles->close();
		}
		(void)m_partition->flush();
	}
}

md_error_t Fat::mount() {
	if (m_mounted) return 0;
	if (!m_buffer) m_buffer.reset(new uint8_t[SECTOR_SIZE]);
	m_fsInfoInvalidated = false;
	md_error_t error = init();
	m_mounted = (error == 0);
	return error;
}

bool Fat::isMounted() const {
	return m_mounted;
}

md_error_t Fat::init() {
	Sector sector(*m_partition, m_buffer.get());
	if (md_error_t error = sector.read(0)) {
		return error;
	}
	// Copied out of the sector: the fields are unaligned
	BPB bpbCopy;
	memcpy(&bpbCopy, sector.data(), sizeof(bpbCopy));
	const BPB *bpb = &bpbCopy;

	// Check BS_jmpBoot
	/* BS_jmpBoot
		Jump instruction to boot code. This field has two allowed forms:
		jmpBoot[0] = 0xEB, jmpBoot[1] = 0x??, jmpBoot[2] = 0x90
			and
		jmpBoot[0] = 0xE9, jmpBoot[1] = 0x??, jmpBoot[2] = 0x??

		0x?? indicates that any 8-bit value is allowed in that byte.
		What this forms is a three-byte Intel x86 unconditional branch
		(jump) instruction that jumps to the start of the operating system
		bootstrap code. This code typically occupies the rest of sector 0 of
		the volume following the BPB and possibly other sectors. Either of
		these forms is acceptable. JmpBoot[0] = 0xEB is the more frequently used format.
	*/
	if(bpb->BS_jmpBoot[0]!=0xEB && bpb->BS_jmpBoot[0]!=0xE9) {
		return Fat_invalidDriveFormat_bs_jmpboot;
	}
	
	/* BPB_BytesPerSec
		Count of bytes per sector. This value may take on only the following
		values: 512, 1024, 2048 or 4096. If maximum compatibility with old
		implementations is desired, only the value 512 should be used. There
		is a lot of FAT code in the world that is basically “hard wired” to 512
		bytes per sector and doesn’t bother to check this field to make sure
		it is 512. Microsoft operating systems will properly support 1024, 2048, and 4096.

		Note: Do not misinterpret these statements about maximum compatibility.
		If the media being recorded has a physical sector size N, you must use N
		and this must still be less than or equal to 4096. Maximum compatibility
		is achieved by only using media with specific sector sizes.
	
		Developer Note: We only support = 512B atm.
	*/
	m_bytesPerSector = bpb->BPB_BytesPerSec;
	if(m_bytesPerSector != 512) {
          return Fat_invalidDriveFormat_sector_size_unsupported;
	}

	/* BPB_SecPerClus
		Number of sectors per allocation unit. This value must be a power of 2
		that is greater than 0. The legal values are 1, 2, 4, 8, 16, 32, 64,
		and 128. Note however, that a value should never be used that results
		in a “bytes per cluster” value (BPB_BytsPerSec * BPB_SecPerClus)
		greater than 32K (32 * 1024). There is a misconception that values
		greater than this are OK. Values that cause a cluster size greater
		than 32K bytes do not work properly; do not try to define one. Some
		versions of some systems allow 64K bytes per cluster value. Many
		application setup programs will not work correctly on such a FAT volume.
	*/
	m_sectorsPerCluster = bpb->BPB_SecPerClus;
    switch (m_sectorsPerCluster) {
        case   1: break;
        case   2: break;
        case   4: break;
        case   8: break;
        case  16: break;
        case  32: break;
        case  64: break;
        case 128: break;
        default:
			return Fat_invalidDriveFormat_sectorsPerCluster;
	}

	/* Note however, that a value should never be used that results in
		a “bytes per cluster” value (BPB_BytsPerSec * BPB_SecPerClus)
		greater than 32K (32 * 1024).
		There is a misconception that values greater than this are OK.
		Values that cause a cluster size greater than 32K bytes do not
		work properly; do not try to define one. Some versions of some
		systems allow 64K bytes per cluster value. Many application setup
		programs will not work correctly on such a FAT volume.
	*/
	m_bytesPerCluster = m_bytesPerSector*m_sectorsPerCluster;
	if (m_bytesPerCluster>32768) {
		// TODO: 64k support?
		return Fat_invalidDriveFormat_cluster_exceeds_32k;
	}

	/* BPB_RsvdSecCnt
		Number of reserved sectors in the Reserved region of the volume
		starting at the first sector of the volume. This field must not
		be 0. For FAT12 and FAT16 volumes, this value should never be
		anything other than 1. For FAT32 volumes, this value is typically 32.
		There is a lot of FAT code in the world “hard wired” to 1 reserved
		sector for FAT12 and FAT16 volumes and that doesn’t bother to check
		this field to make sure it is 1. Microsoft operating systems will
		properly support any non-zero value in this field.
	*/
	if (bpb->BPB_RsvdSecCnt==0) {
		return Fat_invalidDriveFormat_reserved_sectors_is_zero;
	}

	/** BPB_NumFATs
		The count of FAT data structures on the volume. This field should
		always contain the value 2 for any FAT volume of any type. Although
		any value greater than or equal to 1 is perfectly valid, many software
		programs and a few operating systems’ FAT file system drivers may not
		function properly if the value is something other than 2. All Microsoft
		file system drivers will support a value other than 2, but it is still
		highly recommended that no value other than 2 be used in this field.

		The reason the standard value for this field is 2 is to provide
		redundancy for the FAT data structure so that if a sector goes
		bad in one of the FATs, that data is not lost because it is
		duplicated in the other FAT. On non-disk-based media, such as
		FLASH memory cards, where such redundancy is a useless feature,
		a value of 1 may be used to save the space that a second copy of the FAT
		uses, but some FAT file system drivers might not recognize such a volume properly.
	*/
	if (bpb->BPB_NumFATs==0) {
		return Fat_invalidDriveFormat_numfats_must_not_be_zero;
	}

	/* BPB_RootEntCnt
	  For FAT12 and FAT16 volumes, this field contains the count of 32-byte
	  directory entries in the root directory. For FAT32 volumes, this field
	  must be set to 0. For FAT12 and FAT16 volumes, this value should always
	  specify a count that when multiplied by 32 results in an even multiple
	  of BPB_BytsPerSec. For maximum compatibility, FAT16 volumes should use
	  the value 512.

	  Note: there is a check later that for FAT32 that this is 0
     */
	uint32_t rootEntryBytes = static_cast<uint32_t>(bpb->BPB_RootEntCnt) << 5;
	if ( rootEntryBytes & (m_bytesPerSector-1) ) {
		return Fat_invalidDriveFormat_rootEntryCountNotMultipleOfSectorSize;
	}

	/** BPB_TotSec16 (and BPB_TotSec32)
		This field is the old 16-bit total count of sectors on the volume.
		This count includes the count of all sectors in all four regions of
		the volume. This field can be 0; if it is 0, then BPB_TotSec32 must
		be non-zero. For FAT32 volumes, this field must be 0. For FAT12 and
		FAT16 volumes, this field contains the sector count, and BPB_TotSec32
		is 0 if the total sector count “fits” (is less than 0x10000).

		This is not checked as it isn't important.
	*/

	// TODO remove divides
	m_rootDirectorySectorCount = rootEntryBytes / m_bytesPerSector;
	m_sectorsPerFatTable = (bpb->BPB_FATSz16!=0)? bpb->BPB_FATSz16 : bpb->bpb.bpb32.BPB_FATSz32;
	m_sectorCount = (bpb->BPB_TotSec16!=0)? bpb->BPB_TotSec16 : bpb->BPB_TotSec32;

	// The FATs and root directory must fit inside the volume (also guards the
	// subtraction below against wrapping around).
	if (m_sectorsPerFatTable == 0 ||
	    static_cast<uint64_t>(m_sectorCount) <= static_cast<uint64_t>(bpb->BPB_RsvdSecCnt) +
	        static_cast<uint64_t>(bpb->BPB_NumFATs) * m_sectorsPerFatTable + m_rootDirectorySectorCount) {
		return Fat_invalidDriveFormat_volumeTooSmall;
	}
	m_dataSectorCount = m_sectorCount - ( bpb->BPB_RsvdSecCnt + (bpb->BPB_NumFATs*m_sectorsPerFatTable) + m_rootDirectorySectorCount);
	m_firstDataSector = bpb->BPB_RsvdSecCnt + (bpb->BPB_NumFATs * m_sectorsPerFatTable) + m_rootDirectorySectorCount;

	// device->clusters = device->dataSectors >> device->sectorsToClusterShift;
	m_clusterCount = m_dataSectorCount / m_sectorsPerCluster;

	// Every cluster needs a FAT entry: 4 bytes on FAT32, 2 on FAT16, 1.5 on FAT12.
	uint64_t entries = static_cast<uint64_t>(m_clusterCount) + 2;
	uint64_t fatBytesNeeded = m_clusterCount >= 65525 ? entries * 4
		: m_clusterCount >= 4085 ? entries * 2
		: (entries * 3 + 1) / 2;
	if (fatBytesNeeded > static_cast<uint64_t>(m_sectorsPerFatTable) * m_bytesPerSector) {
		return Fat_invalidDriveFormat_fatTooSmall;
	}

	if(m_clusterCount < 4085) {
		m_format = FatFormat::FAT12;
		m_endOfChainCluster = 0x00000FF8;
	}
	else if(m_clusterCount < 65525) {
		m_format = FatFormat::FAT16;
		m_endOfChainCluster = 0x0000FFF8;
	}
	else {
		m_format = FatFormat::FAT32;
		m_endOfChainCluster = 0x0FFFFFF8;
	}

	/* NOT CHECKED
		BPB_SecPerTrk Sectors per track for interrupt 0x13. This field is
		only relevant for media that have a geometry (volume is broken down
		into tracks by multiple heads and cylinders) and are visible on
		interrupt 0x13. This field contains the “sectors per track” geometry value.

		BPB_NumHeads  Number of heads for interrupt 0x13. This field is
		relevant as discussed earlier for BPB_SecPerTrk. This field contains
		the one based “count of heads”. For example, on a 1.44 MB 3.5-inch
		floppy drive this value is 2.

		BPB_HiddSec	  Count of hidden sectors preceding the partition that
		contains this FAT volume. This field is generally only relevant
		for media visible on interrupt 0x13. This field should always be zero
		on media that are not partitioned. Exactly what value is appropriate is
		operating system specific.
	*/

	/********************* FAT12/16 ********************/
	if (m_format <= FatFormat::FAT16) {
		/* BPB_FATSz16 (and BPB_FATSz32)
			On FAT32 volumes this field must be 0, and BPB_FATSz32 contains the FAT size count.
		*/
        /* BS_BootSig
			Extended boot signature (0x29).  This is a signature byte that
			indicates that the following three fields in the boot sector are present.
		*/
		// BS_BootSig isn't checked: it only says whether the volume ID and label are present.
		if (bpb->BPB_FATSz16==0)  {
			return Fat_invalidDriveFormat_rootEntryCountNotMultipleOfSectorSize_FatSz16;
		}
		if (bpb->BPB_RootEntCnt==0) {
			return Fat_invalidDriveFormat_rootEntryCountMustNotBeZero;
		}

		// The root directory is the fixed run of sectors after the FATs
		m_rootSector = bpb->BPB_RsvdSecCnt + bpb->BPB_NumFATs * static_cast<uint32_t>(bpb->BPB_FATSz16);
		m_rootCluster = FIXED_ROOT_CLUSTER;
		m_infoSector = 0;
	}
	/********************* FAT32 ********************/
	else if (m_format == FatFormat::FAT32) {
		// refer BPB_RootEntCnt check
		if (bpb->BPB_RootEntCnt!=0) {
			return Fat_invalidDriveFormat_rootEntryCountMustNotBeZero;
		}

		// refer BPB_TotSec16/32 check
		// Don't care
		// if (bpb->BPB_TotSec16!=0) {}

		/* BPB_FATSz32
			This field is the FAT32 32-bit count of sectors occupied by ONE
			FAT. BPB_FATSz16 must be 0.
		*/
		// Don't care

		if (bpb->bpb.bpb32.BPB_FATSz32==0) {
			return Fat_invalidDriveFormat_rootEntryCountMustNotBeZero_FatSz32;
		}

		/*
			BS_BootSig	Extended boot signature (0x29). This is a
			signature byte that indicates that the following
			three fields in the boot sector are present.
		*/
		/*if (bpb->BS_BootSig==0x29)
		{
			// hmmm nothing to check here
			// but later on when we get/set the label we should check this
		}*/

		// TODO check the max version of BPB_FSVer we support

		if (bpb->bpb.bpb32.BPB_RootClus<2) {
			return Fat_invalidDriveFormat_rootClusterIndex;
		}

		m_rootCluster=bpb->bpb.bpb32.BPB_RootClus;
		m_infoSector= bpb->bpb.bpb32.BPB_FSInfo;
	}

	m_reservedSectors = bpb->BPB_RsvdSecCnt;
	m_numFats = bpb->BPB_NumFATs;

	/*
		If we consider the contents of the sector as a byte array, it must be
		true that sector[510] equals 0x55, and sector[511] equals 0xAA.
	*/
	if (sector.data()[510] != 0x55 || sector.data()[511] != 0xAA) {
		return Fat_invalidDriveFormat_bootBlockEndSignatureNotAA55;
	}

#ifndef MFS_FAT12_16
	// FAT12/16 support (fixed root directory, 12 and 16 bit FAT entries) is a build option
	if (m_format == FatFormat::FAT12) {
		return Fat_Fat12FormatUnsupported;
	}
	if (m_format == FatFormat::FAT16) {
		return Fat_Fat16FormatUnsupported;
	}
#endif

	// Hint for where to start looking for a free cluster.
	m_lastFreeCluster = 2;
	return 0;
}

sector_t Fat::clusterToSector(cluster_t cluster) const {
	return m_firstDataSector + (cluster - 2) * m_sectorsPerCluster;
}

bool Fat::isFixedRoot(cluster_t cluster) const {
#ifdef MFS_FAT12_16
	return cluster == FIXED_ROOT_CLUSTER && m_rootCluster == FIXED_ROOT_CLUSTER;
#else
	(void)cluster;
	return false;
#endif
}

bool Fat::isDataCluster(cluster_t cluster) const {
	return cluster >= 2 && cluster < m_clusterCount + 2;
}

md_error_t Fat::readEntry(FatEntryReader &reader, cluster_t cluster, cluster_t &value) {
#ifdef MFS_FAT12_16
	if (m_format == FatFormat::FAT16) {
		// 2 bytes per entry, 256 per sector
		if (md_error_t error = reader.load(m_reservedSectors + (cluster >> 8))) return error;
		value = readLe16(reader.data() + (cluster & 255) * 2);
		return 0;
	}
	if (m_format == FatFormat::FAT12) {
		// 1.5 bytes per entry, so an entry can start in the last byte of one sector and end in the next
		uint32_t offset = cluster + (cluster >> 1);
		sector_t sector = m_reservedSectors + offset / SECTOR_SIZE;
		uint32_t index = offset % SECTOR_SIZE;
		if (md_error_t error = reader.load(sector)) return error;
		uint32_t pair = reader.data()[index];
		if (index == SECTOR_SIZE - 1) {
			if (md_error_t error = reader.load(sector + 1)) return error;
			pair |= static_cast<uint32_t>(reader.data()[0]) << 8;
		} else {
			pair |= static_cast<uint32_t>(reader.data()[index + 1]) << 8;
		}
		value = (cluster & 1) ? pair >> 4 : pair & 0xFFF;
		return 0;
	}
#endif
	// FAT32: 4 bytes per entry, 128 entries per 512 byte sector
	if (md_error_t error = reader.load(m_reservedSectors + (cluster >> 7))) return error;
	value = readLe32(reader.data() + (cluster & 127) * 4) & FAT32_ENTRY_MASK;
	return 0;
}

/**
 * Given a cluster determine the next cluster.
 */
md_error_t Fat::getEntry(cluster_t cluster, cluster_t &next) {
	next = m_endOfChainCluster;
	if (!m_mounted) return Fat_notMounted;
	// The fixed root directory is one "cluster" with nothing after it
	if (isFixedRoot(cluster)) return 0;
	if (!isDataCluster(cluster)) return Fat_clusterOutOfRange;

	FatEntryReader reader(*m_partition, m_buffer.get());
	cluster_t value;
	if (md_error_t error = readEntry(reader, cluster, value)) return error;
	next = value;
	return 0;
}

md_error_t Fat::writeFatSector(Sector &sector, sector_t sectorInFirstFat) {
	// Keep every FAT copy in sync (mirroring).
	for (uint32_t fat = 0; fat < m_numFats; fat++) {
		if (md_error_t error = sector.write(sectorInFirstFat + fat * m_sectorsPerFatTable)) {
			return error;
		}
	}
	return 0;
}

md_error_t Fat::setEntry(cluster_t cluster, cluster_t next) {
	if (!m_mounted) return Fat_notMounted;
	if (!isDataCluster(cluster)) return Fat_clusterOutOfRange;

	invalidateFsInfo();

	Sector sector(*m_partition, m_buffer.get());
#ifdef MFS_FAT12_16
	if (m_format == FatFormat::FAT16) {
		sector_t sectorOffset = m_reservedSectors + (cluster >> 8);
		if (md_error_t error = sector.read(sectorOffset)) return error;
		writeLe16(sector.data() + (cluster & 255) * 2, static_cast<uint16_t>(next));
		return writeFatSector(sector, sectorOffset);
	}
	if (m_format == FatFormat::FAT12) {
		// Two entries share three bytes: an even cluster has the low byte and the low
		// nibble of the next, an odd one the high nibble of its first byte and all of the next.
		uint32_t value = next & 0xFFF;
		uint32_t offset = cluster + (cluster >> 1);
		sector_t sectorOffset = m_reservedSectors + offset / SECTOR_SIZE;
		uint32_t index = offset % SECTOR_SIZE;
		bool odd = (cluster & 1) != 0;
		if (md_error_t error = sector.read(sectorOffset)) return error;
		uint8_t *first = sector.data() + index;
		*first = odd ? static_cast<uint8_t>((*first & 0x0F) | ((value << 4) & 0xF0)) : static_cast<uint8_t>(value);
		if (index == SECTOR_SIZE - 1) {
			// The entry's second byte is the first of the next sector
			if (md_error_t error = writeFatSector(sector, sectorOffset)) return error;
			sectorOffset++;
			if (md_error_t error = sector.read(sectorOffset)) return error;
		}
		uint8_t *second = sector.data() + (index == SECTOR_SIZE - 1 ? 0 : index + 1);
		*second = odd ? static_cast<uint8_t>(value >> 4) : static_cast<uint8_t>((*second & 0xF0) | ((value >> 8) & 0x0F));
		return writeFatSector(sector, sectorOffset);
	}
#endif
	sector_t sectorOffset = m_reservedSectors + (cluster >> 7);
	if (md_error_t error = sector.read(sectorOffset)) return error;
	uint8_t *entry = sector.data() + (cluster & 127) * 4;
	// The top 4 bits of a FAT32 entry are reserved and must be preserved.
	writeLe32(entry, (readLe32(entry) & ~FAT32_ENTRY_MASK) | (next & FAT32_ENTRY_MASK));
	return writeFatSector(sector, sectorOffset);
}

/**
 * The FSInfo free cluster count is only a hint. Rather than keep it up to date on
 * every allocation we mark it unknown the first time the FAT is modified, which is
 * explicitly allowed by the specification.
 */
void Fat::invalidateFsInfo() {
	if (m_fsInfoInvalidated) return;
	m_fsInfoInvalidated = true;

	if (m_infoSector == 0 || m_infoSector == 0xFFFF) return;

	Sector sector(*m_partition, m_buffer.get());
	if (sector.read(m_infoSector)) return;
	uint8_t *info = sector.data();
	if (readLe32(info + offsetof(FSInfo, FSI_LeadSig)) != FSI_LEAD_SIG ||
	    readLe32(info + offsetof(FSInfo, FSI_StrucSig)) != FSI_STRUC_SIG) {
		return;
	}
	writeLe32(info + offsetof(FSInfo, FSI_Free_Count), 0xFFFFFFFFu);
	writeLe32(info + offsetof(FSInfo, FSI_Nxt_Free), 0xFFFFFFFFu);
	// Only a hint: if it can't be marked unknown, fsck.fat corrects it
	(void)sector.write(m_infoSector);
}

md_error_t Fat::flush() {
	if (!m_mounted) return Fat_notMounted;
	return m_partition->flush();
}

md_error_t Fat::allocateCluster(cluster_t fromCluster, cluster_t &newCluster) {
	newCluster = 0;
	if (!m_mounted) return Fat_notMounted;
	if (isFixedRoot(fromCluster)) {
		// The FAT12/16 root directory can't grow
		return Fat_rootDirectoryFull;
	}

	// Find a free cluster, starting at the hint and wrapping around once. The FAT is
	// read a sector at a time.
	const cluster_t firstCluster = 2;
	const cluster_t endCluster = m_clusterCount + 2;
	cluster_t scanCluster = isDataCluster(m_lastFreeCluster) ? m_lastFreeCluster : firstCluster;

	{
		FatEntryReader reader(*m_partition, m_buffer.get());
		for (uint32_t checked = 0; ; checked++) {
			if (checked >= m_clusterCount) return Fat_partitionIsFull;
			cluster_t value;
			if (md_error_t error = readEntry(reader, scanCluster, value)) return error;
			if (value == FREE_CLUSTER) break;
			if (++scanCluster >= endCluster) scanCluster = firstCluster;
		}
	}

	if (md_error_t error = setEntry(scanCluster, m_endOfChainCluster | 0x7)) return error;

	// Zero the cluster so that new directory clusters contain no stale entries.
	Sector sector(*m_partition, m_buffer.get());
	memset(sector.data(), 0, m_bytesPerSector);
	sector_t first = clusterToSector(scanCluster);
	for (uint32_t i = 0; i < m_sectorsPerCluster; i++) {
		if (md_error_t error = sector.write(first + i)) return error;
	}

	if (fromCluster != 0) {
		if (md_error_t error = setEntry(fromCluster, scanCluster)) return error;
	}

	m_lastFreeCluster = scanCluster + 1;
	newCluster = scanCluster;
	return 0;
}

md_error_t Fat::freeChain(cluster_t cluster) {
	if (!m_mounted) return Fat_notMounted;
	for (uint32_t steps = 0; cluster >= 2 && !isEndOfClusterChain(cluster); steps++) {
		if (steps >= m_clusterCount) return Chain_loopDetected;
		cluster_t next;
		if (md_error_t error = getEntry(cluster, next)) return error;
		if (md_error_t error = setEntry(cluster, FREE_CLUSTER)) return error;
		if (cluster < m_lastFreeCluster) m_lastFreeCluster = cluster;
		cluster = next;
	}
	return 0;
}

md_error_t Fat::countFreeClusters(uint32_t &count) {
	count = 0;
	if (!m_mounted) return Fat_notMounted;
	uint32_t freeCount = 0;
	FatEntryReader reader(*m_partition, m_buffer.get());
	for (cluster_t cluster = 2; cluster < m_clusterCount + 2; cluster++) {
		cluster_t value;
		if (md_error_t error = readEntry(reader, cluster, value)) return error;
		if (value == FREE_CLUSTER) {
			freeCount++;
		}
	}
	count = freeCount;
	return 0;
}

bool Fat::isEndOfClusterChain(cluster_t cluster) const {
	// Anything that is not a valid data cluster terminates the chain
	// (end-of-chain markers, the bad cluster marker, and corrupt values).
	return !isDataCluster(cluster & FAT32_ENTRY_MASK);
}

cluster_t Fat::getEndOfClusterChain() const {
	return m_endOfChainCluster;
}

cluster_t Fat::getRootDirectoryCluster() const {
	return m_rootCluster;
}

uint32_t Fat::getBytesPerCluster() const {
	return m_bytesPerCluster;
}

uint32_t Fat::getClusterBytes(cluster_t cluster) const {
	return isFixedRoot(cluster) ? m_rootDirectorySectorCount * m_bytesPerSector : m_bytesPerCluster;
}

md_error_t Fat::locate(cluster_t cluster, uint32_t offset, uint32_t dataSize, sector_t &sector) const {
	if (!m_mounted) return Fat_notMounted;
	if (!isFixedRoot(cluster) && !isDataCluster(cluster)) return Fat_clusterOutOfRange;
	if (offset + dataSize > getClusterBytes(cluster)) return Fat_CannotAccessBeyondCluster;
	sector = (isFixedRoot(cluster) ? m_rootSector : clusterToSector(cluster)) + offset / m_bytesPerSector;
	return 0;
}

uint32_t Fat::getClusterCount() const {
	return m_clusterCount;
}

FatFormat Fat::getFormat() const {
	return m_format;
}

md_error_t Fat::write(cluster_t cluster, uint32_t offset, const uint8_t *data, uint32_t dataSize) {
	sector_t sectorNumber;
	if (md_error_t error = locate(cluster, offset, dataSize, sectorNumber)) return error;
	uint32_t sectorOffset = offset % m_bytesPerSector;

	Sector sector(*m_partition, m_buffer.get());
	while (dataSize > 0) {
		uint32_t chunk = m_bytesPerSector - sectorOffset;
		if (chunk > dataSize) chunk = dataSize;

		if (chunk == m_bytesPerSector) {
			// A whole sector goes straight from the caller's data
			if (md_error_t error = m_partition->write(data, sectorNumber)) return error;
		} else {
			// A partial sector write must preserve the rest of the sector.
			if (md_error_t error = sector.read(sectorNumber)) return error;
			memcpy(sector.data() + sectorOffset, data, chunk);
			if (md_error_t error = sector.write(sectorNumber)) return error;
		}

		data += chunk;
		dataSize -= chunk;
		sectorOffset = 0;
		sectorNumber++;
	}
	return 0;
}

md_error_t Fat::read(cluster_t cluster, uint32_t offset, uint8_t *data, uint32_t dataSize) {
	sector_t sectorNumber;
	if (md_error_t error = locate(cluster, offset, dataSize, sectorNumber)) return error;
	uint32_t sectorOffset = offset % m_bytesPerSector;

	Sector sector(*m_partition, m_buffer.get());
	while (dataSize > 0) {
		uint32_t chunk = m_bytesPerSector - sectorOffset;
		if (chunk > dataSize) chunk = dataSize;

		if (chunk == m_bytesPerSector) {
			// A whole sector goes straight into the caller's buffer
			if (md_error_t error = m_partition->read(data, sectorNumber)) return error;
		} else {
			if (md_error_t error = sector.read(sectorNumber)) return error;
			memcpy(data, sector.data() + sectorOffset, chunk);
		}

		data += chunk;
		dataSize -= chunk;
		sectorOffset = 0;
		sectorNumber++;
	}
	return 0;
}

MFS_NAMESPACE_END
