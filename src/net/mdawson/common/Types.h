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
#ifdef MFS_THREAD_LOCAL_ERROR
#define MFS_ABI_THREAD_LOCAL _tls
#else
#define MFS_ABI_THREAD_LOCAL
#endif

#define MFS_ABI_JOIN2(a, b, c, d, e) a##b##c##d##e
#define MFS_ABI_JOIN(a, b, c, d, e) MFS_ABI_JOIN2(a, b, c, d, e)
#define MFS_ABI_NAMESPACE MFS_ABI_JOIN(abi, MFS_ABI_LFN, MFS_ABI_CACHE, MFS_ABI_FAT12_16, MFS_ABI_THREAD_LOCAL)

#define MFS_NAMESPACE_BEGIN namespace Mfs { inline namespace MFS_ABI_NAMESPACE {
#define MFS_NAMESPACE_END } }

MFS_NAMESPACE_BEGIN

/**
 * Error code returned by driver functions; 0 means success.
 * (Named md_error_t rather than error_t to avoid clashing with glibc's error_t.)
 */
typedef uint32_t md_error_t;

MFS_NAMESPACE_END

#endif
