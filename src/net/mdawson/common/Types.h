#ifndef net_mdawson_common_Types
#define net_mdawson_common_Types

#include <stdint.h>

/**
 * Everything in the driver is in namespace Mfs.
 *
 * Inside it is an inline namespace named after the feature macros (for example
 * Mfs::abi_lfn_cache), so a program built with different MFS_* macros from the
 * library fails to link instead of running with mismatched class layouts. Code
 * only ever needs to write Mfs::.
 */
#ifdef MFS_LONG_FILE_NAMES
#define MFS_ABI_LFN _lfn
#else
#define MFS_ABI_LFN
#endif
#ifdef MFS_BLOCK_CACHE
#define MFS_ABI_CACHE _cache
#else
#define MFS_ABI_CACHE
#endif
#ifdef MFS_FAT12_16
#define MFS_ABI_FAT12_16 _fat1216
#else
#define MFS_ABI_FAT12_16
#endif

#define MFS_ABI_JOIN2(a, b, c, d) a##b##c##d
#define MFS_ABI_JOIN(a, b, c, d) MFS_ABI_JOIN2(a, b, c, d)
#define MFS_ABI_NAMESPACE MFS_ABI_JOIN(abi, MFS_ABI_LFN, MFS_ABI_CACHE, MFS_ABI_FAT12_16)

#define MFS_NAMESPACE_BEGIN namespace Mfs { inline namespace MFS_ABI_NAMESPACE {
#define MFS_NAMESPACE_END } }

// On-disk structures are declared as packed structs and copied to and from
// sector buffers, which is only right on a little-endian CPU (x86, ARM Cortex-M).
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "The FAT driver needs a little-endian CPU"
#endif

/**
 * On functions that return an md_error_t, so that ignoring one is a compiler
 * warning. Cast the call to void where an error really doesn't matter.
 * [[nodiscard]] from C++17; before that only Clang's attribute, because GCC's
 * can't be silenced with a cast to void.
 */
#if __cplusplus >= 201703L
#define MFS_NODISCARD [[nodiscard]]
#elif defined(__clang__)
#define MFS_NODISCARD __attribute__((warn_unused_result))
#else
#define MFS_NODISCARD
#endif

MFS_NAMESPACE_BEGIN

/**
 * Error code returned by driver functions; 0 means success. The codes are in
 * Error.h. (Named md_error_t rather than error_t to avoid clashing with glibc's
 * error_t.)
 */
using md_error_t = uint32_t;

MFS_NAMESPACE_END

#endif
