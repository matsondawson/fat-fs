#include "File.h"

#include "net/mdawson/filesystem/GlobalError.h"
#include "net/mdawson/filesystem/Chain.h"
#include "net/mdawson/filesystem/LongName.h"

#include <string.h>

MFS_NAMESPACE_BEGIN


	static void defaultFatTime(uint16_t *date, uint16_t *time) {
		*date = (0 << 9) | (1 << 5) | 1; // 1980-01-01
		*time = 0;
	}

	void (*getFatTime)(uint16_t *date, uint16_t *time) = defaultFatTime;

	/** DIR_NTRes flags used by Windows for all lower case 8.3 names. */
	static const uint8_t NTRES_LOWER_BASE = 0x08;
	static const uint8_t NTRES_LOWER_EXT = 0x10;

	/******************************** Names ********************************/

	static const char fatShortFilenameInvalidChars[] = "\"*+,./:;<=>?[\\]|";

	static bool isValidChar(const char ch) {
		// Bytes >= 128 are allowed by the specification (OEM code page characters).
		return (unsigned char)ch > ' ' && ch != 127 && strchr(fatShortFilenameInvalidChars, ch) == 0;
	}

	static char toUpper(char ch) {
		return (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : ch;
	}

	static char toLower(char ch) {
		return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
	}

	bool toShortName(const char *name, uint32_t length, char out[11]) {
		memset(out, ' ', 11);

		if (length == 1 && name[0] == '.') {
			out[0] = '.';
			return true;
		}
		if (length == 2 && name[0] == '.' && name[1] == '.') {
			out[0] = out[1] = '.';
			return true;
		}

		// Already in 11 character directory form? (Upper case only, so that an 11
		// character long name such as "My Document" is not mistaken for one.)
		if (length == 11 && memchr(name, '.', 11) == 0) {
			bool raw = name[0] != ' ';
			for (int i = 0; i < 11 && raw; i++) {
				char ch = name[i];
				if ((ch != ' ' && !isValidChar(ch)) || (ch >= 'a' && ch <= 'z')) raw = false;
			}
			if (raw) {
				memcpy(out, name, 11);
				return true;
			}
		}

		// 8.3 form
		uint32_t i = 0, o = 0;
		for (; i < length && name[i] != '.'; i++) {
			if (o >= 8 || !isValidChar(name[i])) return false;
			out[o++] = toUpper(name[i]);
		}
		if (o == 0) return false;
		if (i < length) {
			i++; // skip '.'
			o = 8;
			for (; i < length; i++) {
				if (o >= 11 || !isValidChar(name[i])) return false;
				out[o++] = toUpper(name[i]);
			}
		}
		// A leading 0xE5 is stored as 0x05.
		if ((unsigned char)out[0] == DIR_ENTRY_DELETED) out[0] = 0x05;
		return true;
	}

	bool toShortName(const char *name, char out[11]) {
		return toShortName(name, strlen(name), out);
	}

	uint32_t formatShortName(const uint8_t shortName[11], uint8_t ntRes, char *out, uint32_t size) {
		if (size == 0) return 0;
		uint32_t o = 0;
		for (int i = 0; i < 8 && shortName[i] != ' ' && o + 1 < size; i++) {
			char ch = (char)shortName[i];
			if (i == 0 && shortName[0] == 0x05) ch = (char)DIR_ENTRY_DELETED;
			out[o++] = (ntRes & NTRES_LOWER_BASE) ? toLower(ch) : ch;
		}
		if (shortName[8] != ' ' && o + 1 < size) {
			out[o++] = '.';
			for (int i = 8; i < 11 && shortName[i] != ' ' && o + 1 < size; i++) {
				char ch = (char)shortName[i];
				out[o++] = (ntRes & NTRES_LOWER_EXT) ? toLower(ch) : ch;
			}
		}
		out[o] = 0;
		return o;
	}

	static cluster_t entryFirstCluster(const DirectoryEntry &entry) {
		return ((uint32_t)entry.DIR_FstClusHI << 16) | entry.DIR_FstClusLO;
	}

	static void setEntryFirstCluster(DirectoryEntry &entry, cluster_t cluster) {
		entry.DIR_FstClusHI = (uint16_t)(cluster >> 16);
		entry.DIR_FstClusLO = (uint16_t)(cluster & 0xFFFF);
	}

	static void initEntry(DirectoryEntry &entry, const char name[11], uint8_t attr) {
		memset(&entry, 0, sizeof(entry));
		memcpy(entry.DIR_Name, name, 11);
		entry.DIR_Attr = attr;
		uint16_t date, time;
		getFatTime(&date, &time);
		entry.DIR_CrtDate = entry.DIR_WrtDate = entry.DIR_LstAccDate = date;
		entry.DIR_CrtTime = entry.DIR_WrtTime = time;
	}

	static bool isLongNameEntry(const DirectoryEntry &entry) {
		return (entry.DIR_Attr & ATTR_LONG_NAME_MASK) == ATTR_LONG_NAME;
	}

	/**
	 * Mark count consecutive directory entries starting at (cluster, offset) deleted.
	 * The entries may continue into following clusters of the directory.
	 */
	static void markEntriesDeleted(Fat &fat, cluster_t cluster, uint32_t offset, uint32_t count) {
		const uint8_t deleted = DIR_ENTRY_DELETED;
		for (uint32_t i = 0; i < count; i++) {
			if (offset >= fat.getClusterBytes(cluster)) {
				cluster = fat.getEntry(cluster);
				if (mdError || fat.isEndOfClusterChain(cluster)) return;
				offset = 0;
			}
			fat.write(cluster, offset, &deleted, 1);
			if (mdError) return;
			offset += sizeof(DirectoryEntry);
		}
	}

	/******************************** File ********************************/

	File::File(Fat& drive, const char* path, const char* name, bool create)
		: m_drive(&drive), m_open(false), m_entryDirty(false), m_entryCluster(0), m_entryClusterOffset(0),
		  m_longNameCluster(0), m_longNameOffset(0), m_longNameEntries(0), m_fileChain(drive) {
		mdError = 0;
		memset(&m_directoryEntry, 0, sizeof(m_directoryEntry));

		if (!drive.isMounted()) {
			mdError = Fat_notMounted;
			return;
		}

		// **** Find directory entry ****
		DirectoryListing dl(drive, path);
		if (mdError) return;

		if (dl.find(name)) {
			if (dl.directoryEntry()->DIR_Attr & ATTR_DIRECTORY) {
				mdError = File_isDirectory;
				return;
			}
		} else {
			if (mdError) return;
			if (!create) {
				mdError = File_fileNotFound;
				return;
			}

			// else create the file...
			dl.createEntry(name, ATTR_ARCHIVE, 0);
			if (mdError) return;
		}

		m_entryCluster = dl.cluster();
		m_entryClusterOffset = dl.clusterOffset();
		m_directoryEntry = *dl.directoryEntry();
		m_longNameCluster = dl.longNameCluster();
		m_longNameOffset = dl.longNameClusterOffset();
		m_longNameEntries = dl.longNameEntryCount();
		m_fileChain.init(entryFirstCluster(m_directoryEntry));
		m_open = true;
	}

	File::~File() {
		if (m_open) {
			md_error_t saved = mdError;
			close();
			if (!mdError) mdError = saved;
		}
	}

	bool File::isOpen() const {
		return m_open;
	}

	uint32_t File::length() const {
		return m_directoryEntry.DIR_FileSize;
	}

	uint32_t File::position() const {
		return m_fileChain.position();
	}

	void File::writeDirectoryEntry() {
		m_drive->write(m_entryCluster, m_entryClusterOffset, (const uint8_t*)&m_directoryEntry, sizeof(DirectoryEntry));
		if (!mdError) m_entryDirty = false;
	}

	void File::touch() {
		uint16_t date, time;
		getFatTime(&date, &time);
		m_directoryEntry.DIR_WrtDate = date;
		m_directoryEntry.DIR_WrtTime = time;
		m_directoryEntry.DIR_LstAccDate = date;
		m_directoryEntry.DIR_Attr |= ATTR_ARCHIVE;
		m_entryDirty = true;
	}

	void File::seek(uint32_t position) {
		mdError = 0;
		if (!m_open) {
			mdError = File_notOpen;
			return;
		}
		if (position > length()) {
			mdError = File_seekExceedsFileSize;
			return;
		}
		m_fileChain.seek(position);
	}

	void File::truncate() {
		mdError = 0;
		if (!m_open) {
			mdError = File_notOpen;
			return;
		}
		m_fileChain.truncate();
		if (mdError) return;
		m_directoryEntry.DIR_FileSize = position();
		setEntryFirstCluster(m_directoryEntry, m_fileChain.firstCluster());
		touch();
		// Freed clusters must not stay referenced by the entry: store it now.
		writeDirectoryEntry();
	}

	void File::flush() {
		mdError = 0;
		if (!m_open) {
			mdError = File_notOpen;
			return;
		}
		if (m_entryDirty) {
			writeDirectoryEntry();
			if (mdError) return;
		}
		m_drive->flush();
	}

	void File::close() {
		if (!m_open) {
			mdError = File_notOpen;
			return;
		}
		flush();
		m_open = false;
	}

	void File::remove() {
		mdError = 0;
		if (!m_open) {
			mdError = File_notOpen;
			return;
		}
		m_fileChain.freeAll();
		if (mdError) return;
		markEntriesDeleted(*m_drive, m_longNameCluster, m_longNameOffset, m_longNameEntries);
		if (mdError) return;
		m_directoryEntry.DIR_Name[0] = DIR_ENTRY_DELETED;
		writeDirectoryEntry();
		m_open = false;
	}

	void File::write(const uint8_t* data, uint32_t bytesToWrite) {
		mdError = 0;
		if (!m_open) {
			mdError = File_notOpen;
			return;
		}
		if (bytesToWrite == 0) return;

		cluster_t firstCluster = m_fileChain.firstCluster();
		m_fileChain.write(data, bytesToWrite);
		md_error_t writeError = mdError;

		// Record whatever was written, even if the write stopped part way (e.g. disk full).
		if (position() > m_directoryEntry.DIR_FileSize) {
			m_directoryEntry.DIR_FileSize = position();
		}
		touch();
		if (m_fileChain.firstCluster() != firstCluster) {
			// The file just got its first cluster. Store the entry straight away so the
			// new chain is never left unreferenced.
			setEntryFirstCluster(m_directoryEntry, m_fileChain.firstCluster());
			mdError = 0;
			writeDirectoryEntry();
		}
		if (writeError) mdError = writeError;
	}

	uint32_t File::read(uint8_t* data, uint32_t bytesToRead) {
		mdError = 0;
		if (!m_open) {
			mdError = File_notOpen;
			return 0;
		}
		uint32_t remaining = length() - position();
		if (bytesToRead > remaining) bytesToRead = remaining;
		return m_fileChain.read(data, bytesToRead);
	}

	/***************************** DirectoryListing *****************************/

	DirectoryListing::DirectoryListing(Fat& drive, const char* path)
		: m_drive(&drive), m_directoryChain(drive), m_entryCluster(0), m_entryClusterOffset(0), m_atEnd(false),
		  m_longNameCluster(0), m_longNameOffset(0), m_longNameEntries(0), m_runTotal(0), m_runNext(0), m_runChecksum(0) {
		mdError = 0;
		memset(&m_directoryEntry, 0, sizeof(m_directoryEntry));
#ifdef MFS_LONG_FILE_NAMES
		m_longName = new uint16_t[LFN_MAX_ENTRIES * LFN_CHARS_PER_ENTRY];
		m_longNameLength = 0;
#endif

		if (!drive.isMounted()) {
			mdError = Fat_notMounted;
			m_startCluster = 0;
			return;
		}

		m_startCluster = drive.getRootDirectoryCluster();
		reset();

		const char *p = path;
		while (*p == '/') p++;
		while (*p) {
			const char *end = p;
			while (*end && *end != '/') end++;

			bool found = find(p, (uint32_t)(end - p));
			if (mdError == File_invalidName) {
				mdError = DirectoryListing_invalidPathFormat;
				return;
			}
			if (mdError) return;
			if (!found || !(m_directoryEntry.DIR_Attr & ATTR_DIRECTORY)) {
				mdError = DirectoryListing_directoryNotFound;
				return;
			}

			// ".." entries pointing at the root directory use cluster 0.
			cluster_t cluster = entryFirstCluster(m_directoryEntry);
			m_startCluster = cluster ? cluster : drive.getRootDirectoryCluster();
			reset();

			p = end;
			while (*p == '/') p++;
		}
	}

	DirectoryListing::~DirectoryListing() {
#ifdef MFS_LONG_FILE_NAMES
		delete[] m_longName;
#endif
	}

	void DirectoryListing::reset() {
		m_directoryChain.init(m_startCluster);
		m_atEnd = false;
		m_longNameEntries = 0;
		resetRun();
	}

	void DirectoryListing::resetRun() {
		m_runTotal = 0;
		m_runNext = 0;
	}

	cluster_t DirectoryListing::startCluster() const {
		return m_startCluster;
	}

#ifdef MFS_LONG_FILE_NAMES
	void DirectoryListing::readLongNameChars(const DirectoryEntryLong &entry, uint32_t index) {
		uint16_t *out = m_longName + index * LFN_CHARS_PER_ENTRY;
		// Fields are packed and may be unaligned: copy them byte wise.
		memcpy(out, entry.LDIR_Name1, sizeof(entry.LDIR_Name1));
		memcpy(out + 5, entry.LDIR_Name2, sizeof(entry.LDIR_Name2));
		memcpy(out + 11, entry.LDIR_Name3, sizeof(entry.LDIR_Name3));
	}
#endif

	bool DirectoryListing::next(bool findEmpty) {
		if (m_atEnd && !findEmpty) return false;

		for (;;) {
			uint32_t bytesRead = m_directoryChain.read((uint8_t*)&m_directoryEntry, sizeof(DirectoryEntry));
			if (mdError || bytesRead != sizeof(DirectoryEntry)) {
				return false;
			}
			// Entries never straddle clusters, so the entry is just behind the cursor.
			m_entryCluster = m_directoryChain.getCluster();
			m_entryClusterOffset = m_directoryChain.clusterOffset() - sizeof(DirectoryEntry);

			uint8_t first = m_directoryEntry.DIR_Name[0];
			if (first == DIR_ENTRY_END) {
				// Free, and no used entries follow.
				resetRun();
				if (findEmpty) return true;
				m_atEnd = true;
				return false;
			}
			if (first == DIR_ENTRY_DELETED) {
				resetRun();
				if (findEmpty) return true;
				continue;
			}
			if (findEmpty) continue;

			if (isLongNameEntry(m_directoryEntry)) {
				const DirectoryEntryLong &entry = *(const DirectoryEntryLong*)&m_directoryEntry;
				uint8_t ordinal = entry.LDIR_Ord & 0x1F;
				if (entry.LDIR_Ord & LFN_LAST_ENTRY) {
					// First physical entry of a run.
					if (ordinal == 0 || ordinal > LFN_MAX_ENTRIES) {
						resetRun();
						continue;
					}
					m_runTotal = ordinal;
					m_runNext = ordinal;
					m_runChecksum = entry.LDIR_Chksum;
					m_longNameCluster = m_entryCluster;
					m_longNameOffset = m_entryClusterOffset;
				} else if (m_runNext == 0 || ordinal != m_runNext || entry.LDIR_Chksum != m_runChecksum) {
					resetRun(); // out of sequence: orphaned entries
					continue;
				}
#ifdef MFS_LONG_FILE_NAMES
				readLongNameChars(entry, ordinal - 1u);
#endif
				m_runNext--;
				continue;
			}

			uint8_t attr = m_directoryEntry.DIR_Attr;
			if (attr & ATTR_VOLUME_ID) {
				resetRun();
				continue;
			}

			// A short entry: does the preceding long name run belong to it?
			m_longNameEntries = 0;
			if (m_runTotal && m_runNext == 0 &&
			    Lfn::shortNameChecksum(m_directoryEntry.DIR_Name) == m_runChecksum) {
				m_longNameEntries = m_runTotal;
#ifdef MFS_LONG_FILE_NAMES
				uint32_t maxLength = (uint32_t)m_runTotal * LFN_CHARS_PER_ENTRY;
				m_longNameLength = 0;
				while (m_longNameLength < maxLength && m_longName[m_longNameLength] != 0) m_longNameLength++;
				if (m_longNameLength == 0 || m_longNameLength > LFN_MAX_LENGTH) m_longNameEntries = 0;
#endif
			}
			resetRun();
			return true;
		}
	}

	bool DirectoryListing::hasLongName() const {
#ifdef MFS_LONG_FILE_NAMES
		return m_longNameEntries != 0;
#else
		return false;
#endif
	}

	uint8_t DirectoryListing::longNameEntryCount() const {
		return m_longNameEntries;
	}

	cluster_t DirectoryListing::longNameCluster() const {
		return m_longNameCluster;
	}

	uint32_t DirectoryListing::longNameClusterOffset() const {
		return m_longNameOffset;
	}

	uint32_t DirectoryListing::name(char *out, uint32_t size) {
#ifdef MFS_LONG_FILE_NAMES
		if (m_longNameEntries) {
			return Lfn::toUtf8(m_longName, m_longNameLength, out, size);
		}
#endif
		return formatShortName(m_directoryEntry.DIR_Name, m_directoryEntry.DIR_NTRes, out, size);
	}

	bool DirectoryListing::find(const char *name) {
		return find(name, strlen(name));
	}

	bool DirectoryListing::find(const char *name, uint32_t length) {
		char shortName[11];
		bool isShortName = toShortName(name, length, shortName);

#ifdef MFS_LONG_FILE_NAMES
		uint16_t *wanted = new uint16_t[LFN_MAX_LENGTH];
		int32_t wantedLength = Lfn::fromUtf8(name, length, wanted);
		if (!isShortName && wantedLength <= 0) {
			delete[] wanted;
			mdError = File_invalidName;
			return false;
		}
		reset();
		bool found = false;
		while (next()) {
			if (isShortName && memcmp(m_directoryEntry.DIR_Name, shortName, 11) == 0) { found = true; break; }
			if (wantedLength > 0 && m_longNameEntries &&
			    Lfn::equalsIgnoreCase(m_longName, m_longNameLength, wanted, (uint32_t)wantedLength)) {
				found = true;
				break;
			}
		}
		delete[] wanted;
		return found;
#else
		if (!isShortName) {
			mdError = File_invalidName;
			return false;
		}
		return findShortName(shortName);
#endif
	}

	bool DirectoryListing::findShortName(const char shortName[11]) {
		reset();
		while (next()) {
			if (memcmp(m_directoryEntry.DIR_Name, shortName, 11) == 0) {
				return true;
			}
		}
		return false;
	}

	DirectoryEntry* DirectoryListing::directoryEntry() {
		return &m_directoryEntry;
	}

	cluster_t DirectoryListing::cluster() {
		return m_entryCluster;
	}

	uint32_t DirectoryListing::clusterOffset() {
		return m_entryClusterOffset;
	}

	void DirectoryListing::writeEntries(const DirectoryEntry *entries, uint32_t count) {
		mdError = 0;

		// Find count consecutive free slots (they may span clusters).
		reset();
		cluster_t runCluster = 0;
		uint32_t runOffset = 0, runLength = 0;
		cluster_t lastCluster = m_startCluster;
		DirectoryEntry entry;
		for (;;) {
			uint32_t n = m_directoryChain.read((uint8_t*)&entry, sizeof(entry));
			if (mdError) return;
			if (n != sizeof(entry)) break; // end of the directory's chain
			lastCluster = m_directoryChain.getCluster();
			uint8_t first = entry.DIR_Name[0];
			if (first == DIR_ENTRY_END || first == DIR_ENTRY_DELETED) {
				if (runLength == 0) {
					runCluster = lastCluster;
					runOffset = m_directoryChain.clusterOffset() - sizeof(entry);
				}
				if (++runLength == count) break;
			} else {
				runLength = 0;
			}
		}

		// Use the run found, or the free slots at the end continued into new clusters.
		cluster_t cluster;
		uint32_t offset;
		if (runLength > 0) {
			cluster = runCluster;
			offset = runOffset;
		} else {
			cluster = lastCluster;
			offset = m_drive->getClusterBytes(cluster); // forces a new cluster below
		}

		// Two passes over the slots: the first makes sure they all exist, growing the
		// directory as needed, so that a directory that can't grow (a full disk, or the
		// fixed FAT12/16 root directory) fails before any entry is written.
		const cluster_t startCluster = cluster;
		const uint32_t startOffset = offset;
		for (int pass = 0; pass < 2; pass++) {
			cluster = startCluster;
			offset = startOffset;
			for (uint32_t i = 0; i < count; i++) {
				if (offset >= m_drive->getClusterBytes(cluster)) {
					// A full FAT12/16 root directory fails here, with Fat_rootDirectoryFull
					cluster_t next = m_drive->getEntry(cluster);
					if (mdError) return;
					if (m_drive->isEndOfClusterChain(next)) {
						next = m_drive->allocateCluster(cluster); // zeroed, so it reads as free
						if (mdError) return;
					}
					cluster = next;
					offset = 0;
				}
				if (pass == 1) {
					if (i == 0) {
						m_longNameCluster = cluster;
						m_longNameOffset = offset;
					}
					m_drive->write(cluster, offset, (const uint8_t*)&entries[i], sizeof(DirectoryEntry));
					if (mdError) return;
					m_entryCluster = cluster;
					m_entryClusterOffset = offset;
				}
				offset += sizeof(DirectoryEntry);
			}
		}

		m_directoryEntry = entries[count - 1];
		m_longNameEntries = (uint8_t)(count - 1);
		// The chain cursor is no longer meaningful: the next next() starts over.
		reset();
		m_longNameEntries = (uint8_t)(count - 1);
	}

	void DirectoryListing::addEntry(const DirectoryEntry &entry) {
		writeEntries(&entry, 1);
	}

	void DirectoryListing::addFileEntry(const char* name) {
		DirectoryEntry entry;
		initEntry(entry, name, ATTR_ARCHIVE);
		addEntry(entry);
	}

	/** True if name is plain ASCII with no lower case, i.e. exactly representable as 8.3. */
	static bool isPlainShortName(const char *name, uint32_t length) {
		for (uint32_t i = 0; i < length; i++) {
			unsigned char ch = (unsigned char)name[i];
			if (ch >= 0x80 || (ch >= 'a' && ch <= 'z')) return false;
		}
		return true;
	}

	void DirectoryListing::createEntry(const char *name, uint8_t attr, cluster_t firstCluster) {
		mdError = 0;
		uint32_t length = strlen(name);

		char shortName[11];
		bool isShortName = toShortName(name, length, shortName) && shortName[0] != '.';

#ifdef MFS_LONG_FILE_NAMES
		if (!isShortName || !isPlainShortName(name, length)) {
			uint16_t *longName = new uint16_t[LFN_MAX_LENGTH];
			int32_t longLength = Lfn::fromUtf8(name, length, longName);
			if (longLength <= 0 || (longLength <= 2 && longName[0] == '.' && longName[longLength - 1] == '.')) {
				delete[] longName;
				mdError = File_invalidName;
				return;
			}

			// Choose a unique short alias.
			char basis[11];
			bool lossy;
			Lfn::makeBasisName(longName, (uint32_t)longLength, basis, &lossy);
			bool unique = false;
			if (!lossy) {
				memcpy(shortName, basis, 11);
				unique = !findShortName(shortName);
			}
			for (uint32_t n = 1; !unique && !mdError && n <= 999999; n++) {
				Lfn::applyNumericTail(basis, n, shortName);
				unique = !findShortName(shortName);
			}
			if (mdError || !unique) {
				delete[] longName;
				if (!mdError) mdError = File_alreadyExists;
				return;
			}

			// Long name entries (last part first) followed by the short entry.
			uint32_t longEntries = ((uint32_t)longLength + LFN_CHARS_PER_ENTRY - 1) / LFN_CHARS_PER_ENTRY;
			DirectoryEntry *entries = new DirectoryEntry[longEntries + 1];
			uint8_t checksum = Lfn::shortNameChecksum((const uint8_t*)shortName);
			for (uint32_t e = 0; e < longEntries; e++) {
				uint32_t ordinal = longEntries - e;
				uint16_t chars[LFN_CHARS_PER_ENTRY];
				for (uint32_t k = 0; k < LFN_CHARS_PER_ENTRY; k++) {
					uint32_t position = (ordinal - 1) * LFN_CHARS_PER_ENTRY + k;
					if (position < (uint32_t)longLength) chars[k] = longName[position];
					else if (position == (uint32_t)longLength) chars[k] = 0x0000;
					else chars[k] = 0xFFFF;
				}
				DirectoryEntryLong *entry = (DirectoryEntryLong*)&entries[e];
				memset(entry, 0, sizeof(*entry));
				entry->LDIR_Ord = (uint8_t)(ordinal | (e == 0 ? LFN_LAST_ENTRY : 0));
				memcpy(entry->LDIR_Name1, chars, sizeof(entry->LDIR_Name1));
				memcpy(entry->LDIR_Name2, chars + 5, sizeof(entry->LDIR_Name2));
				memcpy(entry->LDIR_Name3, chars + 11, sizeof(entry->LDIR_Name3));
				entry->LDIR_Attr = ATTR_LONG_NAME;
				entry->LDIR_Type = 0;
				entry->LDIR_Chksum = checksum;
				entry->LDIR_FstClusLO = 0;
			}
			initEntry(entries[longEntries], shortName, attr);
			setEntryFirstCluster(entries[longEntries], firstCluster);

			writeEntries(entries, longEntries + 1);
			if (!mdError) {
				memcpy(m_longName, longName, (uint32_t)longLength * sizeof(uint16_t));
				m_longNameLength = (uint32_t)longLength;
			}
			delete[] entries;
			delete[] longName;
			return;
		}
#else
		(void)isPlainShortName;
#endif
		if (!isShortName) {
			mdError = File_invalidName;
			return;
		}
		DirectoryEntry entry;
		initEntry(entry, shortName, attr);
		setEntryFirstCluster(entry, firstCluster);
		writeEntries(&entry, 1);
	}

	/******************************** Directories ********************************/

	void makeDirectory(Fat &drive, const char *path, const char *name) {
		mdError = 0;

		DirectoryListing parent(drive, path);
		if (mdError) return;
		if (parent.find(name)) {
			mdError = File_alreadyExists;
			return;
		}
		if (mdError) return;

		cluster_t cluster = drive.allocateCluster(0);
		if (mdError) return;

		// "." and ".." entries; ".." is cluster 0 when the parent is the root.
		DirectoryEntry dots[2];
		initEntry(dots[0], ".          ", ATTR_DIRECTORY);
		setEntryFirstCluster(dots[0], cluster);
		initEntry(dots[1], "..         ", ATTR_DIRECTORY);
		cluster_t parentCluster = parent.startCluster();
		setEntryFirstCluster(dots[1], parentCluster == drive.getRootDirectoryCluster() ? 0 : parentCluster);
		drive.write(cluster, 0, (const uint8_t*)dots, sizeof(dots));
		if (mdError) return;

		parent.createEntry(name, ATTR_DIRECTORY, cluster);
		if (mdError) {
			// freeChain stops at an error already set, so clear it while freeing
			md_error_t error = mdError;
			mdError = 0;
			drive.freeChain(cluster);
			mdError = error;
		}
	}

MFS_NAMESPACE_END
