#include "Partition.h"

#include "net/mdawson/filesystem/Sector.h"
#include "net/mdawson/filesystem/GlobalError.h"

#pragma pack(1)

typedef struct // Partition entry
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
} PartitionEntry_t;

typedef struct // Partition table
{
  PartitionEntry_t partitions[4];
  uint16_t bootBlockSignature;
} PartitionTable_t;

#pragma pack()

md_error_t Partition::decodePartition(uint32_t partition) {
	// set m_partitionSectorOffset to that of first partition
	// set m_partitionSectorCount

	PartitionTable_t *partitionTable;
	
	// Found the partition so return OK
	if (partition==7) return 0;
	
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
  
	// partition table offset==0x1BE
	// todo confirm is partition table and not Boot block
	partitionTable = (PartitionTable_t*)(&(partitionSector.m_data)[446]);
	if (partitionTable->bootBlockSignature == 0xAA55) // correct signature?
	{
		// disk is not referenced from this ...
		PartitionEntry_t *partitionEntry = &(partitionTable->partitions[partition&3]);
		if (partitionEntry->type == 0 || partitionEntry->sectorCount == 0) {
			// Empty slot
			return Partition_tableInvalid;
		}
		m_sectorOffset += partitionEntry->sectorsBetweenMbrAndFirstSector;
		m_sectorCount = partitionEntry->sectorCount;
		return decodePartition(partition>>3);
	}
 
	return Partition_tableInvalid;
}

Partition::Partition(SectorDevice &sectorDevice, uint32_t partition) {
	m_sectorDevice = &sectorDevice;
	m_sectorOffset = 0;
	// Until a partition table limits it, the whole device is addressable.
	m_sectorCount = (sector_t)-1;

	mdError = decodePartition(partition);
}


Partition::~Partition(void) {
}

md_error_t Partition::read(uint8_t* data, sector_t sector) {
	if (sector>=m_sectorCount) {
		return Partition_sectorOutOfRange;
	}

	return m_sectorDevice->read(data, sector+m_sectorOffset);
}

md_error_t Partition::write(const uint8_t* data, sector_t sector) {
	if (sector>=m_sectorCount) {
		return Partition_sectorOutOfRange;
	}

	return m_sectorDevice->write(data, sector+m_sectorOffset);
}

uint32_t Partition::getBytesPerSector() const {
	return m_sectorDevice->getBytesPerSector();
}
md_error_t Partition::flush() {
	return m_sectorDevice->flush();
}
