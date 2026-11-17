#ifndef net_mdawson_common_Types
#define net_mdawson_common_Types

#include <stdint.h>

/**
 * Error code returned by driver functions; 0 means success.
 * (Named md_error_t rather than error_t to avoid clashing with glibc's error_t.)
 */
typedef uint32_t md_error_t;

#endif
