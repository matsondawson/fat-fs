#ifndef net_mdawson_filesystems_File
#define net_mdawson_filesystems_File

#include "net/mdawson/common/Types.h"
#include "net/mdawson/filesystem/Fat.h"
#include "net/mdawson/filesystem/SectorDevice.h"
#include "net/mdawson/filesystem/Chain.h"
#include "net/mdawson/filesystem/LongName.h"

#pragma pack(1)

typedef struct // FAT 32 Byte Directory Entry Structure
{
  /*
          DIR_Name	0	11	Short name.
  */
  uint8_t DIR_Name[11];

  /*
          DIR_Attr	11	1	File attributes:
          ATTR_READ_ONLY   	0x01
          ATTR_HIDDEN 	0x02
          ATTR_SYSTEM 	0x04
          ATTR_VOLUME_ID 	0x08
          ATTR_DIRECTORY	0x10
          ATTR_ARCHIVE  	0x20
          ATTR_LONG_NAME 	ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID
          The upper two bits of the attribute byte are reserved and should always be set to 0 when a file is created and never modified or looked at after that.
  */
  uint8_t DIR_Attr;

  /*
          DIR_NTRes	12	1	Reserved for use by Windows NT. Set value to 0 when a file is created and never modify or look at it after that.
  */
  uint8_t DIR_NTRes;

  /*
          DIR_CrtTimeTenth	13	1	Millisecond stamp at file creation time. This field actually contains a count of tenths of a second. The granularity of the seconds part of DIR_CrtTime is 2 seconds so this field is a count of tenths of a second and its valid value range is 0-199 inclusive.
  */
  uint8_t DIR_CrtTimeTenth;

  /*
          DIR_CrtTime	14	2	Time file was created.
  */
  uint16_t DIR_CrtTime;

  /*
          DIR_CrtDate	16	2	Date file was created.
  */
  uint16_t DIR_CrtDate;

  /*
          DIR_LstAccDate	18	2	Last access date. Note that there is no last access time, only a date. This is the date of last read or write. In the case of a write, this should be set to the same date as DIR_WrtDate.
  */
  uint16_t DIR_LstAccDate;

  /*
          DIR_FstClusHI	20	2	High word of this entry’s first cluster number (always 0 for a FAT12 or FAT16 volume).
  */
  uint16_t DIR_FstClusHI;

  /*
          DIR_WrtTime	22	2	Time of last write. Note that file creation is considered a write.
  */
  uint16_t DIR_WrtTime;

  /*
          DIR_WrtDate	24	2	Date of last write. Note that file creation is considered a write.
  */
  uint16_t DIR_WrtDate;

  /*
          DIR_FstClusLO	26	2	Low word of this entry’s first cluster number.
  */
  uint16_t DIR_FstClusLO;

  /*
          DIR_FileSize	28	4	32-bit DWORD holding this file’s size in bytes.
  */
  uint32_t DIR_FileSize;

} DirectoryEntry;

typedef struct
{
  // 	0	1	The order of this entry in the sequence of long dir entries associated
  // with the short dir entry at the end of the long dir set.
  // If masked with 0x40 (LAST_LONG_ENTRY), this indicates the entry is the last long
  // dir entry in a set of long dir entries. All valid sets of long dir entries must
  // begin with an entry having this mask.
  uint8_t LDIR_Ord;

  // 1	10	Characters 1-5 of the long-name sub-component in this dir entry.
  uint16_t LDIR_Name1[5];

  // 11	1	Attributes - must be ATTR_LONG_NAME
  uint8_t LDIR_Attr;
  
  // 12	1	If zero, indicates a directory entry that is a sub-component of a long name.
  // NOTE: Other values reserved for future extensions.
  // Non-zero implies other dirent types.
  uint8_t LDIR_Type;

  // 13	1	Checksum of name in the short dir entry at the end of the long dir set.
  uint8_t LDIR_Chksum;
  
  // 14	12	Characters 6-11 of the long-name sub-component in this dir entry.
  uint16_t LDIR_Name2[6];

  // 26	2	Must be ZERO. This is an artifact of the FAT "first cluster"
  // and must be zero for compatibility with existing disk utilities.  It's meaningless in the context of a long dir entry.
  uint16_t LDIR_FstClusLO;

  // 28	4	Characters 12-13 of the long-name sub-component in this dir entry.
  uint16_t LDIR_Name3[2];

} DirectoryEntryLong;

#pragma pack()

static_assert(sizeof(DirectoryEntry) == 32, "DirectoryEntry must be 32 bytes");
static_assert(sizeof(DirectoryEntryLong) == 32, "DirectoryEntryLong must be 32 bytes");

namespace Mfs {
	/**
	 * Convert a name to the 11 character, space padded, upper case directory form,
	 * e.g. "test.txt" -> "TEST    TXT". A name that is already 11 characters with
	 * no '.' is taken to be in directory form and is only validated.
	 * "." and ".." are accepted.
	 *
	 * @return false if the name is not a valid 8.3 short name.
	 */
	bool toShortName(const char *name, char out[11]);
	bool toShortName(const char *name, uint32_t length, char out[11]);

	/**
	 * Source of timestamps for created/modified entries. Defaults to 1980-01-01 00:00
	 * (microcontrollers often have no clock); set it to supply real time.
	 * date = ((year-1980)<<9)|(month<<5)|day, time = (hour<<11)|(minute<<5)|(seconds/2)
	 */
	extern void (*getFatTime)(uint16_t *date, uint16_t *time);

	/**
	 * Format an 11 byte directory name as "NAME.EXT" (honouring the lower case
	 * flags Windows stores in DIR_NTRes). @return length written.
	 */
	uint32_t formatShortName(const uint8_t shortName[11], uint8_t ntRes, char *out, uint32_t size);

	/**
	 * Representation of an entry in the file system.
	 *
	 * Paths are '/' separated, e.g. "/" or "/SUB/DIR". Each component and the name may
	 * be written normally ("test.txt") or in 11 character directory form ("TEST    TXT").
	 * With MFS_LONG_FILE_NAMES defined, long names ("My Long Name.txt", UTF-8) can be
	 * used anywhere and are created when a name is not a plain upper case 8.3 name;
	 * matching is case insensitive. Without it only 8.3 names are supported.
	 *
	 * The file size in the directory entry is updated in memory by write() and stored
	 * by flush(), close() or the destructor. Call flush() when data must survive a
	 * power loss.
	 *
	 * Check mdError (or isOpen()) after construction.
	 */
	class File {
	private:
		Fat *m_drive;
		bool m_open;
		bool m_entryDirty;

		// Directory entry information
		cluster_t m_entryCluster;
		uint32_t m_entryClusterOffset;
		DirectoryEntry m_directoryEntry;
		// Long name entries belonging to this file (deleted together with it)
		cluster_t m_longNameCluster;
		uint32_t m_longNameOffset;
		uint8_t m_longNameEntries;

		// File information
		Chain m_fileChain;

		void writeDirectoryEntry();
		void touch();

	public:
		/**
		 * Open (and if create is true, create if not present) the file name in directory path.
		 *
		 * @param path Directory containing the file, e.g. "/" or "/SUB"; mandatory.
		 * @param name File name, e.g. "TEST.TXT"; mandatory.
		 * @param create true if file is to be created if not present, else false.
		 */
		File(Fat &drive, const char* path, const char *name, bool create = false);
		/** Closes (and so flushes) the file. */
		~File();

		File(const File&) = delete;
		File& operator=(const File&) = delete;

		bool isOpen() const;

		/**
		 * Read data from the file at the current position.
		 *
		 * @param data Where to put the result; mandatory.
		 * @param bytesToRead The number of bytes to read maximum; mandatory.
		 * @return The number of bytes read (less than requested at end of file).
		 */
		uint32_t read(uint8_t* data, uint32_t bytesToRead);

		/**
		 * Write data to the file at the current position, growing the file as needed.
		 *
		 * @param data Where to read the data from; mandatory.
		 * @param bytesToWrite Number of bytes to write; mandatory.
		 */
		void write(const uint8_t* data, uint32_t bytesToWrite);

		/**
		 * Seek to an position in a file (0 to length() inclusive).
		 */
		void seek(uint32_t position);

		/**
		 * Get the length of the file.
		 */
		uint32_t length() const;

		/**
		 * Get the current position in the file.
		 */
		uint32_t position() const;

		/**
		 * Truncate a file at the current position.
		 */
		void truncate();

		/**
		 * Store the directory entry (size, times) if it changed and flush the device
		 * (e.g. a write-back cache), so that everything written so far is on the media.
		 */
		void flush();

		/** Flush and close. Further operations fail with File_notOpen. */
		void close();

		/**
		 * Delete a file.  Called remove as delete is reserved. The File is closed afterwards.
		 */
		void remove();
	};

	/**
	 * Listing of directory entrys is done through here.
	 * Deleted entries, long file name entries and the volume label are skipped.
	 */
	class DirectoryListing {
	private:
		Fat *m_drive;
		Chain m_directoryChain;
		cluster_t m_startCluster;
		cluster_t m_entryCluster;
		uint32_t m_entryClusterOffset;
		bool m_atEnd;

		DirectoryEntry m_directoryEntry;

		// Long name run before the current entry. Tracked even without long name
		// support so that deleting an entry can delete its long name entries too.
		cluster_t m_longNameCluster;
		uint32_t m_longNameOffset;
		uint8_t m_longNameEntries;   // long name entries belonging to the current entry
		uint8_t m_runTotal;          // entries in the run being read (0 = none)
		uint8_t m_runNext;           // next expected ordinal
		uint8_t m_runChecksum;
#ifdef MFS_LONG_FILE_NAMES
		uint16_t *m_longName;        // heap, LFN_MAX_ENTRIES * 13 units
		uint32_t m_longNameLength;
		void readLongNameChars(const DirectoryEntryLong &entry, uint32_t index);
#endif

		void resetRun();
		/** Write count entries to consecutive free slots, growing the directory as needed. */
		void writeEntries(const DirectoryEntry *entries, uint32_t count);

	public:
		/**
		 * @param path Directory to list, e.g. "/" or "/SUB". Sets mdError if not found.
		 */
		DirectoryListing(Fat &drive, const char* path);
		~DirectoryListing();
		DirectoryListing(const DirectoryListing&) = delete;
		DirectoryListing& operator=(const DirectoryListing&) = delete;

		/** Go back to the first entry of this directory. */
		void reset();

		/**
		 * Move to the next entry.
		 * @param findEmpty If true, move to the next free slot instead.
		 * @return false at the end of the listing.
		 */
		bool next(bool findEmpty = false);

		/** Find an entry by name (short, 11 character or long form). @return false if not found. */
		bool find(const char *name);
		bool find(const char *name, uint32_t length);
		/** Find an entry by its 11 character directory form name (not null terminated). */
		bool findShortName(const char shortName[11]);

		/** Location of the current entry. */
		cluster_t cluster();
		uint32_t clusterOffset();
		DirectoryEntry *directoryEntry();

		/**
		 * Name of the current entry as UTF-8: the long name if there is one (and long
		 * names are enabled), otherwise "NAME.EXT". @return length.
		 */
		uint32_t name(char *out, uint32_t size);

		/** True if the current entry has a (valid) long name. */
		bool hasLongName() const;

		/** Long name entries preceding the current entry, and where they start. */
		uint8_t longNameEntryCount() const;
		cluster_t longNameCluster() const;
		uint32_t longNameClusterOffset() const;

		/** First cluster of the directory being listed. */
		cluster_t startCluster() const;

		/**
		 * Create a new entry called name, with long name entries if needed.
		 * Does not check whether name already exists. Afterwards the current entry is the new entry.
		 */
		void createEntry(const char *name, uint8_t attr, cluster_t firstCluster);

		/**
		 * Store entry in the first free slot, growing the directory if needed.
		 * Afterwards the current entry is the new entry.
		 */
		void addEntry(const DirectoryEntry &entry);

		/** Add an empty file entry; name in 11 character directory form. */
		void addFileEntry(const char* name);
	};

	/**
	 * Create directory name inside directory path.
	 */
	void makeDirectory(Fat &drive, const char *path, const char *name);
}

#endif
