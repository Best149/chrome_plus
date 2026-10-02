/*
 * Wrapper that exposes only miniz's declarations (no implementation), so that
 * miniz.c can also be compiled as its own translation unit without producing
 * duplicate symbols.
 */
#ifdef __cplusplus
extern "C" {
#endif

#define MINIZ_HEADER_FILE_ONLY
#include "miniz.c"


#ifdef __cplusplus
}
#endif