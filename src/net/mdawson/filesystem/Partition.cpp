#include "Partition.h"

#include "net/mdawson/filesystem/Sector.h"
#include "net/mdawson/filesystem/Endian.h"
#include "net/mdawson/filesystem/Error.h"

#include <string.h>

MFS_NAMESPACE_BEGIN

#pragma pack(1)

struct PartitionEntry
{
  // 00h Current State of Partition (00h=Inactive, 80h=Active) 1 Byte
  uint8_t state;
  
  // 01h Beginning of Partition - Head 1 Byte
  uint8_t headPartitionBegin;
  
  // 02h Beginning of Partition - Cylinder/Sector (See Below) 1 Word
  uint16_t cylinderOrSectorPartitionBegin;
  
  // 04h Type of Partition (See List Below) 1 Byte
  uint8_t type;
  
  // 05h End of Partition - Head 1 Byte
  uint8_t headPartitionEnd;	
  
  // 06h End of Partition - Cylinder/Sector 1 Word
  uint16_t cylinderOrSectorPartitionEnd;
  
  // 08h Number of Sectors Between the MBR and the First Sector in the Partition 1 Double Word
  uint32_t sectorsBetweenMbrAndFirstSector;
  
  // 0Ch Number of Sectors in the Partition 1 Double Word
  uint32_t sectorCount;
};

#pragma pack()

static_assert(sizeof(PartitionEntry) == 16, "PartitionEntry must match the on-disk layout");

/** Offsets in the MBR sector. */
constexpr uint32_t PARTITION_TABLE_OFFSET = 446;
constexpr uint32_t BOOT_SIGNATURE_OFFSET = 510;

md_error_t Partition::decodePartition(uint32_t partition) {
	// Found the partition so return OK
	if (partition == WHOLE_DEVICE) return 0;

	Sector partitionSector(*m_sectorDevice);
	if (md_error_t error = partitionSector.read(m_sectorOffset)) {
		return error;
	}

	// TODO: Have not implemented detection of non-partitioned devices
	// hacky here, if buffer[0] is 0 then we are at an MBR not a boot sector

	// This is a biggy here, this 0th sector may actually be a Partition record
	// I determine this by checking end bytes 0x55 and 0xAA, checking for not all
	// 0's after byte 446.

	// Technically a floppy disk doesn't have this MBR it has a Fat BR
	// so to be as compatible as possible I try to detect if it's an MBR or FBR
	// by checking whether there are any Partition records (i.e. not all 0's)
	// if there are then assume it's an MBR

	// todo confirm is partition table and not Boot block
	if (readLe16(partitionSector.data() + BOOT_SIGNATURE_OFFSET) == 0xAA55) // correct signature?
	{
		PartitionEntry entry;
		memcpy(&entry, partitionSector.data() + PARTITION_TABLE_OFFSET + (partition & 3) * sizeof(PartitionEntry),
		       sizeof(entry));
		if (entry.type == 0 || entry.sectorCount == 0) {
			// Empty slot
			return Partition_tableInvalid;
		}
		m_sectorOffset += entry.sectorsBetweenMbrAndFirstSector;
		m_sectorCount = entry.sectorCount;
		return decodePartition(partition >> 3);
	}

	return Partition_tableInvalid;
}

Partition::Partition(SectorDevice &sectorDevice)
	: m_sectorDevice(&sectorDevice), m_sectorOffset(0), m_sectorCount(0), m_open(false) {
}

md_error_t Partition::open(uint32_t partition) {
	m_sectorOffset = 0;
	// Until a partition table limits it, the whole device is addressable.
	m_sectorCount = static_cast<sector_t>(-1);
	md_error_t error = decodePartition(partition);
	m_open = (error == 0);
	if (!m_open) m_sectorCount = 0;
	return error;
}

md_error_t Partition::read(uint8_t* data, sector_t sector) {
	if (!m_open) return Partition_notOpen;
	if (sector >= m_sectorCount) return Partition_sectorOutOfRange;
	return m_sectorDevice->read(data, sector + m_sectorOffset);
}

md_error_t Partition::write(const uint8_t* data, sector_t sector) {
	if (!m_open) return Partition_notOpen;
	if (sector >= m_sectorCount) return Partition_sectorOutOfRange;
	return m_sectorDevice->write(data, sector + m_sectorOffset);
}

uint32_t Partition::getBytesPerSector() const {
	return m_sectorDevice->getBytesPerSector();
}

md_error_t Partition::flush() {
	return m_sectorDevice->flush();
}

MFS_NAMESPACE_END
