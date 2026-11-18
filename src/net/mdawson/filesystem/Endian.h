#ifndef net_mdawson_filesystem_Endian
#define net_mdawson_filesystem_Endian

#include "net/mdawson/common/Types.h"

MFS_NAMESPACE_BEGIN

/**
 * Little-endian loads and stores at any alignment. On-disk fields are read and
 * written through these (or memcpy) rather than by casting a byte pointer to
 * another type, which C++ doesn't allow.
 */
inline uint16_t readLe16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t readLe32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
	       (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void writeLe16(uint8_t *p, uint16_t value) {
	p[0] = static_cast<uint8_t>(value);
	p[1] = static_cast<uint8_t>(value >> 8);
}

inline void writeLe32(uint8_t *p, uint32_t value) {
	p[0] = static_cast<uint8_t>(value);
	p[1] = static_cast<uint8_t>(value >> 8);
	p[2] = static_cast<uint8_t>(value >> 16);
	p[3] = static_cast<uint8_t>(value >> 24);
}

MFS_NAMESPACE_END

#endif
