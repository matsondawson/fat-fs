#include "LongName.h"

MFS_NAMESPACE_BEGIN

	namespace Lfn {

		uint8_t shortNameChecksum(const uint8_t shortName[11]) {
			uint8_t sum = 0;
			for (int i = 0; i < 11; i++) {
				sum = static_cast<uint8_t>(((sum & 1) ? 0x80 : 0) + (sum >> 1) + shortName[i]);
			}
			return sum;
		}

#ifdef MFS_LONG_FILE_NAMES

		static bool isInvalidLongNameChar(uint32_t ch) {
			if (ch < 0x20) return true;
			switch (ch) {
				case '"': case '*': case '/': case ':': case '<':
				case '>': case '?': case '\\': case '|':
					return true;
			}
			return false;
		}

		int32_t fromUtf8(const char *name, uint32_t length, uint16_t *out) {
			const uint8_t *s = reinterpret_cast<const uint8_t*>(name);
			uint32_t i = 0, o = 0;
			while (i < length) {
				uint32_t ch = s[i];
				uint32_t extra;
				if (ch < 0x80) { extra = 0; }
				else if ((ch & 0xE0) == 0xC0) { ch &= 0x1F; extra = 1; }
				else if ((ch & 0xF0) == 0xE0) { ch &= 0x0F; extra = 2; }
				else if ((ch & 0xF8) == 0xF0) { ch &= 0x07; extra = 3; }
				else return -1;
				if (i + extra >= length + (extra ? 0 : 1)) return -1;
				for (uint32_t k = 1; k <= extra; k++) {
					if ((s[i + k] & 0xC0) != 0x80) return -1;
					ch = (ch << 6) | (s[i + k] & 0x3F);
				}
				// Reject overlong encodings, surrogates and out of range values.
				static const uint32_t minimum[4] = { 0, 0x80, 0x800, 0x10000 };
				if (ch < minimum[extra] || ch > 0x10FFFF || (ch >= 0xD800 && ch <= 0xDFFF)) return -1;
				i += extra + 1;

				if (isInvalidLongNameChar(ch)) return -1;
				if (ch >= 0x10000) {
					if (o + 2 > LFN_MAX_LENGTH) return -1;
					ch -= 0x10000;
					out[o++] = static_cast<uint16_t>(0xD800 | (ch >> 10));
					out[o++] = static_cast<uint16_t>(0xDC00 | (ch & 0x3FF));
				} else {
					if (o + 1 > LFN_MAX_LENGTH) return -1;
					out[o++] = static_cast<uint16_t>(ch);
				}
			}
			// Trailing spaces and periods are not significant.
			while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
			if (o == 0) return -1;
			return static_cast<int32_t>(o);
		}

		uint32_t toUtf8(const uint16_t *name, uint32_t length, char *out, uint32_t size) {
			if (size == 0) return 0;
			uint32_t o = 0;
			for (uint32_t i = 0; i < length; i++) {
				uint32_t ch = name[i];
				if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < length &&
				    name[i + 1] >= 0xDC00 && name[i + 1] <= 0xDFFF) {
					ch = 0x10000 + ((ch - 0xD800) << 10) + (name[i + 1] - 0xDC00);
					i++;
				} else if (ch >= 0xD800 && ch <= 0xDFFF) {
					ch = 0xFFFD; // unpaired surrogate
				}
				uint8_t bytes[4];
				uint32_t n;
				if (ch < 0x80) { bytes[0] = static_cast<uint8_t>(ch); n = 1; }
				else if (ch < 0x800) { bytes[0] = static_cast<uint8_t>(0xC0 | (ch >> 6)); bytes[1] = static_cast<uint8_t>(0x80 | (ch & 0x3F)); n = 2; }
				else if (ch < 0x10000) {
					bytes[0] = static_cast<uint8_t>(0xE0 | (ch >> 12)); bytes[1] = static_cast<uint8_t>(0x80 | ((ch >> 6) & 0x3F));
					bytes[2] = static_cast<uint8_t>(0x80 | (ch & 0x3F)); n = 3;
				} else {
					bytes[0] = static_cast<uint8_t>(0xF0 | (ch >> 18)); bytes[1] = static_cast<uint8_t>(0x80 | ((ch >> 12) & 0x3F));
					bytes[2] = static_cast<uint8_t>(0x80 | ((ch >> 6) & 0x3F)); bytes[3] = static_cast<uint8_t>(0x80 | (ch & 0x3F)); n = 4;
				}
				if (o + n + 1 > size) break;
				for (uint32_t k = 0; k < n; k++) out[o++] = static_cast<char>(bytes[k]);
			}
			out[o] = 0;
			return o;
		}

		static uint16_t asciiUpper(uint16_t ch) {
			return (ch >= 'a' && ch <= 'z') ? static_cast<uint16_t>(ch - 'a' + 'A') : ch;
		}

		bool equalsIgnoreCase(const uint16_t *a, uint32_t aLength, const uint16_t *b, uint32_t bLength) {
			if (aLength != bLength) return false;
			for (uint32_t i = 0; i < aLength; i++) {
				if (asciiUpper(a[i]) != asciiUpper(b[i])) return false;
			}
			return true;
		}

		/** Convert one character for a short name; sets lossy if it can't be represented. */
		static char shortChar(uint16_t ch, bool &lossy) {
			if (ch >= 0x80) { lossy = true; return '_'; }
			switch (ch) {
				case '+': case ',': case ';': case '=': case '[': case ']':
					lossy = true;
					return '_';
			}
			return static_cast<char>(asciiUpper(ch));
		}

		void makeBasisName(const uint16_t *name, uint32_t length, char out[11], bool &lossy) {
			for (int i = 0; i < 11; i++) out[i] = ' ';
			lossy = false;

			// Leading periods and spaces are dropped.
			uint32_t start = 0;
			while (start < length && (name[start] == '.' || name[start] == ' ')) {
				start++;
				lossy = true;
			}
			// The extension follows the last period.
			int32_t lastDot = -1;
			for (uint32_t i = start; i < length; i++) {
				if (name[i] == '.') lastDot = static_cast<int32_t>(i);
			}
			uint32_t baseEnd = lastDot >= 0 ? static_cast<uint32_t>(lastDot) : length;

			uint32_t o = 0;
			for (uint32_t i = start; i < baseEnd; i++) {
				if (name[i] == ' ' || name[i] == '.') { lossy = true; continue; }
				if (o >= 8) { lossy = true; break; }
				out[o++] = shortChar(name[i], lossy);
			}
			if (lastDot >= 0) {
				o = 8;
				for (uint32_t i = static_cast<uint32_t>(lastDot) + 1; i < length; i++) {
					if (name[i] == ' ') { lossy = true; continue; }
					if (o >= 11) { lossy = true; break; }
					out[o++] = shortChar(name[i], lossy);
				}
			}
			if (out[0] == ' ') {
				out[0] = '_';
				lossy = true;
			}
		}

		void applyNumericTail(const char basis[11], uint32_t n, char out[11]) {
			char tail[8];
			uint32_t tailLength = 0;
			char digits[7];
			uint32_t digitCount = 0;
			do {
				digits[digitCount++] = static_cast<char>('0' + n % 10);
				n /= 10;
			} while (n && digitCount < 6);
			tail[tailLength++] = '~';
			while (digitCount) tail[tailLength++] = digits[--digitCount];

			uint32_t baseLength = 0;
			while (baseLength < 8 && basis[baseLength] != ' ') baseLength++;
			if (baseLength > 8 - tailLength) baseLength = 8 - tailLength;

			for (int i = 0; i < 11; i++) out[i] = ' ';
			for (uint32_t i = 0; i < baseLength; i++) out[i] = basis[i];
			for (uint32_t i = 0; i < tailLength; i++) out[baseLength + i] = tail[i];
			for (int i = 8; i < 11; i++) out[i] = basis[i];
		}
#endif
	}

MFS_NAMESPACE_END
