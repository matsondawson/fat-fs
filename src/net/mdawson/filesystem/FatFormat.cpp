#include "FatFormat.h"

#include "net/mdawson/filesystem/FatStructures.h"
#include "net/mdawson/filesystem/Endian.h"
#include "net/mdawson/filesystem/Error.h"
#include "net/mdawson/filesystem/Sector.h"

#include <stddef.h>
#include <string.h>

MFS_NAMESPACE_BEGIN


	constexpr uint32_t RESERVED_SECTORS = 32;
	constexpr uint32_t NUM_FATS = 2;
	constexpr uint32_t ROOT_CLUSTER = 2;
	constexpr uint32_t FSINFO_SECTOR = 1;
	constexpr uint32_t BACKUP_BOOT_SECTOR = 6;
	constexpr uint8_t MEDIA_FIXED = 0xF8;

	static uint8_t defaultSectorsPerCluster(sector_t sectorCount) {
		// Microsoft's recommended FAT32 cluster sizes.
		if (sectorCount <= 532480) return 1;      // up to 260MB: 512 bytes
		if (sectorCount <= 16777216) return 8;    // up to 8GB: 4K
		if (sectorCount <= 33554432) return 16;   // up to 16GB: 8K
		if (sectorCount <= 67108864) return 32;   // up to 32GB: 16K
		return 64;                                // 32K
	}

	static void setLabel(uint8_t out[11], const char *label) {
		memset(out, ' ', 11);
		const char *volumeLabel = label ? label : "NO NAME";
		for (int i = 0; i < 11 && volumeLabel[i]; i++) {
			char ch = volumeLabel[i];
			out[i] = static_cast<uint8_t>((ch >= 'a' && ch <= 'z') ? ch - 'a' + 'A' : ch);
		}
	}

	md_error_t formatFat32(SectorDevice &device, sector_t sectorCount, uint8_t sectorsPerCluster,
	                       const char *label, uint32_t volumeId) {
		if (sectorsPerCluster == 0) {
			sectorsPerCluster = defaultSectorsPerCluster(sectorCount);
		}
		if (sectorsPerCluster > 64 || (sectorsPerCluster & (sectorsPerCluster - 1)) != 0) {
			return Fat_formatInvalidClusterSize;
		}
		if (sectorCount <= RESERVED_SECTORS) {
			return Fat_formatVolumeTooSmall;
		}

		// FAT size calculation from the Microsoft FAT specification.
		uint32_t tmp1 = sectorCount - RESERVED_SECTORS;
		uint32_t tmp2 = ((256u * sectorsPerCluster) + NUM_FATS) / 2;
		uint32_t fatSize = (tmp1 + tmp2 - 1) / tmp2;

		uint32_t dataSectors = sectorCount - RESERVED_SECTORS - NUM_FATS * fatSize;
		uint32_t clusterCount = dataSectors / sectorsPerCluster;
		if (clusterCount < 65525) {
			return Fat_formatVolumeTooSmall;
		}

		Sector sector(device);

		// Clear the reserved region and the FATs.
		memset(sector.data(), 0, SECTOR_SIZE);
		for (sector_t s = 0; s < RESERVED_SECTORS + NUM_FATS * fatSize; s++) {
			if (md_error_t error = sector.write(s)) return error;
		}
		// Clear the root directory cluster.
		sector_t firstDataSector = RESERVED_SECTORS + NUM_FATS * fatSize;
		for (uint32_t s = 0; s < sectorsPerCluster; s++) {
			if (md_error_t error = sector.write(firstDataSector + (ROOT_CLUSTER - 2) * sectorsPerCluster + s)) return error;
		}

		// Boot sector
		BPB bpb;
		memset(&bpb, 0, sizeof(bpb));
		bpb.BS_jmpBoot[0] = 0xEB;
		bpb.BS_jmpBoot[1] = 0x58;
		bpb.BS_jmpBoot[2] = 0x90;
		memcpy(bpb.BS_OEMName, "MSWIN4.1", 8);
		bpb.BPB_BytesPerSec = SECTOR_SIZE;
		bpb.BPB_SecPerClus = sectorsPerCluster;
		bpb.BPB_RsvdSecCnt = RESERVED_SECTORS;
		bpb.BPB_NumFATs = NUM_FATS;
		bpb.BPB_RootEntCnt = 0;
		bpb.BPB_TotSec16 = 0;
		bpb.BPB_Media = MEDIA_FIXED;
		bpb.BPB_FATSz16 = 0;
		bpb.BPB_SecPerTrk = 63;
		bpb.BPB_NumHeads = 255;
		bpb.BPB_HiddSec = 0;
		bpb.BPB_TotSec32 = sectorCount;
		bpb.bpb.bpb32.BPB_FATSz32 = fatSize;
		bpb.bpb.bpb32.BPB_ExtFlags = 0;
		bpb.bpb.bpb32.BPB_FSVer = 0;
		bpb.bpb.bpb32.BPB_RootClus = ROOT_CLUSTER;
		bpb.bpb.bpb32.BPB_FSInfo = FSINFO_SECTOR;
		bpb.bpb.bpb32.BPB_BkBootSec = BACKUP_BOOT_SECTOR;
		bpb.bpb.bpb32.BS_DrvNum = 0x80;
		bpb.bpb.bpb32.BS_BootSig = 0x29;
		bpb.bpb.bpb32.BS_VolID = volumeId;
		setLabel(bpb.bpb.bpb32.BS_VolLab, label);
		memcpy(bpb.bpb.bpb32.BS_FilSysType, "FAT32   ", 8);
		memcpy(sector.data(), &bpb, sizeof(bpb));
		sector.data()[510] = 0x55;
		sector.data()[511] = 0xAA;
		if (md_error_t error = sector.write(0)) return error;
		if (md_error_t error = sector.write(BACKUP_BOOT_SECTOR)) return error;

		// FSInfo
		uint8_t *info = sector.data();
		memset(info, 0, SECTOR_SIZE);
		writeLe32(info + offsetof(FSInfo, FSI_LeadSig), FSI_LEAD_SIG);
		writeLe32(info + offsetof(FSInfo, FSI_StrucSig), FSI_STRUC_SIG);
		writeLe32(info + offsetof(FSInfo, FSI_Free_Count), clusterCount - 1); // root directory uses one cluster
		writeLe32(info + offsetof(FSInfo, FSI_Nxt_Free), ROOT_CLUSTER + 1);
		writeLe32(info + offsetof(FSInfo, FSI_TrailSig), FSI_TRAIL_SIG);
		if (md_error_t error = sector.write(FSINFO_SECTOR)) return error;
		if (md_error_t error = sector.write(BACKUP_BOOT_SECTOR + 1)) return error;

		// First FAT sector: media descriptor, reserved entry, root directory end-of-chain.
		uint8_t *fat = sector.data();
		memset(fat, 0, SECTOR_SIZE);
		writeLe32(fat, 0x0FFFFF00u | MEDIA_FIXED);
		writeLe32(fat + 4, 0x0FFFFFFFu);
		writeLe32(fat + ROOT_CLUSTER * 4, 0x0FFFFFFFu);
		for (uint32_t f = 0; f < NUM_FATS; f++) {
			if (md_error_t error = sector.write(RESERVED_SECTORS + f * fatSize)) return error;
		}
		return 0;
	}

#ifdef MFS_FAT12_16
	constexpr uint32_t SMALL_RESERVED_SECTORS = 1;
	constexpr uint32_t ROOT_ENTRIES = 512;

	struct SmallLayout {
		uint32_t fatSize;
		uint32_t clusterCount;
	};

	// FAT size and cluster count, growing the FAT until it holds an entry for every cluster
	static bool smallLayout(sector_t sectorCount, uint32_t sectorsPerCluster, bool fat12, SmallLayout &layout) {
		const uint32_t rootSectors = ROOT_ENTRIES * 32 / SECTOR_SIZE;
		uint32_t fatSize = 1;
		for (;;) {
			uint32_t overhead = SMALL_RESERVED_SECTORS + NUM_FATS * fatSize + rootSectors;
			if (sectorCount <= overhead) return false;
			uint32_t clusters = (sectorCount - overhead) / sectorsPerCluster;
			uint32_t bytes = fat12 ? ((clusters + 2) * 3 + 1) / 2 : (clusters + 2) * 2;
			uint32_t needed = (bytes + SECTOR_SIZE - 1) / SECTOR_SIZE;
			if (needed <= fatSize) {
				layout.fatSize = fatSize;
				layout.clusterCount = clusters;
				return true;
			}
			fatSize = needed;
		}
	}

	static md_error_t formatSmall(SectorDevice &device, sector_t sectorCount, bool fat12, uint8_t sectorsPerCluster,
	                        const char *label, uint32_t volumeId) {
		const uint32_t minClusters = fat12 ? 1 : 4085;
		const uint32_t maxClusters = fat12 ? 4084 : 65524;
		SmallLayout layout;

		if (sectorsPerCluster == 0) {
			// The smallest cluster that keeps the count within the type's limit
			for (sectorsPerCluster = 1; ; sectorsPerCluster *= 2) {
				if (!smallLayout(sectorCount, sectorsPerCluster, fat12, layout) || layout.clusterCount < minClusters) {
					return Fat_formatVolumeTooSmall;
				}
				if (layout.clusterCount <= maxClusters) break;
				if (sectorsPerCluster == 64) {
					return Fat_formatVolumeTooLarge;
				}
			}
		} else {
			if (sectorsPerCluster > 64 || (sectorsPerCluster & (sectorsPerCluster - 1)) != 0) {
				return Fat_formatInvalidClusterSize;
			}
			if (!smallLayout(sectorCount, sectorsPerCluster, fat12, layout) || layout.clusterCount < minClusters) {
				return Fat_formatVolumeTooSmall;
			}
			if (layout.clusterCount > maxClusters) {
				return Fat_formatVolumeTooLarge;
			}
		}

		const uint32_t rootSectors = ROOT_ENTRIES * 32 / SECTOR_SIZE;
		Sector sector(device);

		// Clear the reserved sector, the FATs and the root directory.
		memset(sector.data(), 0, SECTOR_SIZE);
		for (sector_t s = 0; s < SMALL_RESERVED_SECTORS + NUM_FATS * layout.fatSize + rootSectors; s++) {
			if (md_error_t error = sector.write(s)) return error;
		}

		// Boot sector
		BPB bpb;
		memset(&bpb, 0, sizeof(bpb));
		bpb.BS_jmpBoot[0] = 0xEB;
		bpb.BS_jmpBoot[1] = 0x3C;
		bpb.BS_jmpBoot[2] = 0x90;
		memcpy(bpb.BS_OEMName, "MSWIN4.1", 8);
		bpb.BPB_BytesPerSec = SECTOR_SIZE;
		bpb.BPB_SecPerClus = sectorsPerCluster;
		bpb.BPB_RsvdSecCnt = SMALL_RESERVED_SECTORS;
		bpb.BPB_NumFATs = NUM_FATS;
		bpb.BPB_RootEntCnt = ROOT_ENTRIES;
		bpb.BPB_TotSec16 = sectorCount < 0x10000 ? static_cast<uint16_t>(sectorCount) : 0;
		bpb.BPB_Media = MEDIA_FIXED;
		bpb.BPB_FATSz16 = static_cast<uint16_t>(layout.fatSize);
		bpb.BPB_SecPerTrk = 63;
		bpb.BPB_NumHeads = 255;
		bpb.BPB_HiddSec = 0;
		bpb.BPB_TotSec32 = sectorCount < 0x10000 ? 0 : sectorCount;
		bpb.bpb.bpb16.BS_DrvNum = 0x80;
		bpb.bpb.bpb16.BS_Reserved1 = 0;
		bpb.bpb.bpb16.BS_BootSig = 0x29;
		bpb.bpb.bpb16.BS_VolID = volumeId;
		setLabel(bpb.bpb.bpb16.BS_VolLab, label);
		memcpy(bpb.bpb.bpb16.BS_FilSysType, fat12 ? "FAT12   " : "FAT16   ", 8);
		memcpy(sector.data(), &bpb, sizeof(bpb));
		sector.data()[510] = 0x55;
		sector.data()[511] = 0xAA;
		if (md_error_t error = sector.write(0)) return error;

		// First FAT sector: the media descriptor and the reserved entry 1
		memset(sector.data(), 0, SECTOR_SIZE);
		sector.data()[0] = MEDIA_FIXED;
		sector.data()[1] = 0xFF;
		sector.data()[2] = 0xFF;
		if (!fat12) sector.data()[3] = 0xFF;
		for (uint32_t f = 0; f < NUM_FATS; f++) {
			if (md_error_t error = sector.write(SMALL_RESERVED_SECTORS + f * layout.fatSize)) return error;
		}
		return 0;
	}
#endif

	md_error_t formatFat(SectorDevice &device, sector_t sectorCount, FatFormat format, uint8_t sectorsPerCluster,
	                     const char *label, uint32_t volumeId) {
		if (format == FatFormat::FAT32) {
			return formatFat32(device, sectorCount, sectorsPerCluster, label, volumeId);
		}
#ifdef MFS_FAT12_16
		return formatSmall(device, sectorCount, format == FatFormat::FAT12, sectorsPerCluster, label, volumeId);
#else
		(void)device; (void)sectorCount; (void)sectorsPerCluster; (void)label; (void)volumeId;
		return format == FatFormat::FAT12 ? Fat_Fat12FormatUnsupported : Fat_Fat16FormatUnsupported;
#endif
	}

MFS_NAMESPACE_END
