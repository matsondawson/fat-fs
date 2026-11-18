#ifndef net_mdawson_filesystems_LongName
#define net_mdawson_filesystems_LongName

#include "net/mdawson/common/Types.h"

MFS_NAMESPACE_BEGIN

/**
 * Long file name (VFAT) helpers.
 *
 * Long names are stored on disk as UTF-16 in a run of 32 byte entries placed
 * immediately before the 8.3 "short" entry they belong to. The API uses UTF-8.
 *
 * Long name support is compiled in when MFS_LONG_FILE_NAMES is defined
 * (CMake option FAT_LONG_FILE_NAMES). shortNameChecksum() is always available
 * because deleting a file must also delete any long name entries it has.
 */

/** Maximum long name length in UTF-16 code units. */
constexpr uint32_t LFN_MAX_LENGTH = 255;
/** Characters stored per long name entry. */
constexpr uint32_t LFN_CHARS_PER_ENTRY = 13;
/** Maximum long name entries for one name: ceil(255 / 13). */
constexpr uint32_t LFN_MAX_ENTRIES = 20;
/** Set on the ordinal of the first physical (last logical) long name entry. */
constexpr uint32_t LFN_LAST_ENTRY = 0x40;

	namespace Lfn {
		/** Checksum of an 11 byte short name, stored in each of its long name entries. */
		uint8_t shortNameChecksum(const uint8_t shortName[11]);

#ifdef MFS_LONG_FILE_NAMES
		/**
		 * Convert a UTF-8 name to a valid long name in UTF-16: trailing spaces and
		 * periods are removed, and characters not allowed in long names are rejected.
		 *
		 * @param out Buffer of at least LFN_MAX_LENGTH units.
		 * @return length in UTF-16 units, or -1 if the name is not valid.
		 */
		int32_t fromUtf8(const char *name, uint32_t length, uint16_t *out);

		/**
		 * Convert UTF-16 to null terminated UTF-8.
		 * @return bytes written excluding the terminator (truncated to fit size).
		 */
		uint32_t toUtf8(const uint16_t *name, uint32_t length, char *out, uint32_t size);

		/** Compare two UTF-16 names, ignoring ASCII case. */
		bool equalsIgnoreCase(const uint16_t *a, uint32_t aLength, const uint16_t *b, uint32_t bLength);

		/**
		 * Make the 8.3 basis name for a long name (Microsoft's algorithm): upper case,
		 * spaces and extra periods removed, unrepresentable characters become '_',
		 * truncated to 8.3. lossy is set when the basis does not represent the name
		 * exactly (ignoring case), meaning a numeric tail ("~1") is required.
		 */
		void makeBasisName(const uint16_t *name, uint32_t length, char out[11], bool &lossy);

		/** Apply numeric tail n to a basis name, e.g. ("LONGFILE", "TXT"), 1 -> "LONGFI~1TXT". */
		void applyNumericTail(const char basis[11], uint32_t n, char out[11]);
#endif
	}

MFS_NAMESPACE_END

#endif
