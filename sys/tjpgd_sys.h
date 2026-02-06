#ifndef TJPGD_SYS_H
#define TJPGD_SYS_H

#include "tjpgd.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Prepare JPEG decoder
 * @note Weak symbol - can be overridden for testing
 */
//__attribute__((weak))
JRESULT tjpgd_sys_prepare(
    JDEC* jd,
    size_t (*infunc)(JDEC*, uint8_t*, size_t),
    void* pool,
    size_t sz_pool,
    void* dev
);

/**
 * @brief Decompress JPEG image
 * @note Weak symbol - can be overridden for testing
 */
//__attribute__((weak))
JRESULT tjpgd_sys_decomp(
    JDEC* jd,
    int (*outfunc)(JDEC*, void*, JRECT*),
    uint8_t scale
);

#ifdef __cplusplus
}
#endif

#endif // TJPGD_SYS_H