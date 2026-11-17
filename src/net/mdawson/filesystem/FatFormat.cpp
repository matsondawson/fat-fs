#include "FatFormat.h"

#include "net/mdawson/filesystem/FatStructures.h"
#include "net/mdawson/filesystem/GlobalError.h"
#include "net/mdawson/filesystem/Sector.h"

#include <string.h>

namespace Mfs {

	static const uint32_t RESERVED_SECTORS = 32;
	static const uint32_t NUM_FATS = 2;
	static const uint32_t ROOT_CLUSTER = 2;
	static const uint32_t FSINFO_SECTOR = 1;
	static const uint32_t BACKUP_BOOT_SECTOR = 6;
	static const uint8_t MEDIA_FIXED = 0xF8;

	static uint8_t defaultSectorsPerCluster(sector_t sectorCount) {
		// Microsoft's recommended FAT32 cluster sizes.
		if (sectorCount <= 532480) return 1;      // up to 260MB: 512 bytes
		if (sectorCount <= 16777216) return 8;    // up to 8GB: 4K
		if (sectorCount <= 33554432) return 16;   // up to 16GB: 8K
		if (sectorCount <= 67108864) return 32;   // up to 32GB: 16K
		return 64;                                // 32K
	}

	static bool writeSector(Sector &sector, sector_t number) {
		if (md_error_t error = sector.write(number)) {
			mdError = error;
			return false;
		}
		return true;
	}

	void formatFat32(SectorDevice &device, sector_t sectorCount, uint8_t sectorsPerCluster,
	                 const char *label, uint32_t volumeId) {
		mdError = 0;

		if (sectorsPerCluster == 0) {
			sectorsPerCluster = defaultSectorsPerCluster(sectorCount);
		}
		if (sectorsPerCluster > 64 || (sectorsPerCluster & (sectorsPerCluster - 1)) != 0) {
			mdError = Fat_formatInvalidClusterSize;
			return;
		}
		if (sectorCount <= RESERVED_SECTORS) {
			mdError = Fat_formatVolumeTooSmall;
			return;
		}

		// FAT size calculation from the Microsoft FAT specification.
		uint32_t tmp1 = sectorCount - RESERVED_SECTORS;
		uint32_t tmp2 = ((256u * sectorsPerCluster) + NUM_FATS) / 2;
		uint32_t fatSize = (tmp1 + tmp2 - 1) / tmp2;

		uint32_t dataSectors = sectorCount - RESERVED_SECTORS - NUM_FATS * fatSize;
		uint32_t clusterCount = dataSectors / sectorsPerCluster;
		if (clusterCount < 65525) {
			mdError = Fat_formatVolumeTooSmall;
			return;
		}

		Sector sector(device);

		// Clear the reserved region and the FATs.
		memset(sector.m_data, 0, SECTOR_SIZE);
		for (sector_t s = 0; s < RESERVED_SECTORS + NUM_FATS * fatSize; s++) {
			if (!writeSector(sector, s)) return;
		}
		// Clear the root directory cluster.
		sector_t firstDataSector = RESERVED_SECTORS + NUM_FATS * fatSize;
		for (uint32_t s = 0; s < sectorsPerCluster; s++) {
			if (!writeSector(sector, firstDataSector + (ROOT_CLUSTER - 2) * sectorsPerCluster + s)) return;
		}

		// Boot sector
		BPB_t *bpb = (BPB_t*)sector.m_data;
		bpb->BS_jmpBoot[0] = 0xEB;
		bpb->BS_jmpBoot[1] = 0x58;
		bpb->BS_jmpBoot[2] = 0x90;
		memcpy(bpb->BS_OEMName, "MSWIN4.1", 8);
		bpb->BPB_BytesPerSec = SECTOR_SIZE;
		bpb->BPB_SecPerClus = sectorsPerCluster;
		bpb->BPB_RsvdSecCnt = RESERVED_SECTORS;
		bpb->BPB_NumFATs = NUM_FATS;
		bpb->BPB_RootEntCnt = 0;
		bpb->BPB_TotSec16 = 0;
		bpb->BPB_Media = MEDIA_FIXED;
		bpb->BPB_FATSz16 = 0;
		bpb->BPB_SecPerTrk = 63;
		bpb->BPB_NumHeads = 255;
		bpb->BPB_HiddSec = 0;
		bpb->BPB_TotSec32 = sectorCount;
		bpb->bpb.bpb32.BPB_FATSz32 = fatSize;
		bpb->bpb.bpb32.BPB_ExtFlags = 0;
		bpb->bpb.bpb32.BPB_FSVer = 0;
		bpb->bpb.bpb32.BPB_RootClus = ROOT_CLUSTER;
		bpb->bpb.bpb32.BPB_FSInfo = FSINFO_SECTOR;
		bpb->bpb.bpb32.BPB_BkBootSec = BACKUP_BOOT_SECTOR;
		bpb->bpb.bpb32.BS_DrvNum = 0x80;
		bpb->bpb.bpb32.BS_BootSig = 0x29;
		bpb->bpb.bpb32.BS_VolID = volumeId;
		memset(bpb->bpb.bpb32.BS_VolLab, ' ', 11);
		const char *volumeLabel = label ? label : "NO NAME";
		for (int i = 0; i < 11 && volumeLabel[i]; i++) {
			char ch = volumeLabel[i];
			bpb->bpb.bpb32.BS_VolLab[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
		}
		memcpy(bpb->bpb.bpb32.BS_FilSysType, "FAT32   ", 8);
		sector.m_data[510] = 0x55;
		sector.m_data[511] = 0xAA;
		if (!writeSector(sector, 0)) return;
		if (!writeSector(sector, BACKUP_BOOT_SECTOR)) return;

		// FSInfo
		memset(sector.m_data, 0, SECTOR_SIZE);
		FSInfo_t *info = (FSInfo_t*)sector.m_data;
		info->FSI_LeadSig = FSI_LEAD_SIG;
		info->FSI_StrucSig = FSI_STRUC_SIG;
		info->FSI_Free_Count = clusterCount - 1; // root directory uses one cluster
		info->FSI_Nxt_Free = ROOT_CLUSTER + 1;
		info->FSI_TrailSig = FSI_TRAIL_SIG;
		if (!writeSector(sector, FSINFO_SECTOR)) return;
		if (!writeSector(sector, BACKUP_BOOT_SECTOR + 1)) return;

		// First FAT sector: media descriptor, reserved entry, root directory end-of-chain.
		memset(sector.m_data, 0, SECTOR_SIZE);
		uint32_t *fat = (uint32_t*)sector.m_data;
		fat[0] = 0x0FFFFF00u | MEDIA_FIXED;
		fat[1] = 0x0FFFFFFFu;
		fat[ROOT_CLUSTER] = 0x0FFFFFFFu;
		for (uint32_t f = 0; f < NUM_FATS; f++) {
			if (!writeSector(sector, RESERVED_SECTORS + f * fatSize)) return;
		}
	}
}
