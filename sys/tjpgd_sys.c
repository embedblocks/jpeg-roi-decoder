#include "tjpgd_sys.h"

__attribute__((weak))
JRESULT tjpgd_sys_prepare(
    JDEC* jd,
    size_t (*infunc)(JDEC*, uint8_t*, size_t),
    void* pool,
    size_t sz_pool,
    void* dev)
{
    return jd_prepare(jd, infunc, pool, sz_pool, dev);
}

__attribute__((weak))
JRESULT tjpgd_sys_decomp(
    JDEC* jd,
    int (*outfunc)(JDEC*, void*, JRECT*),
    uint8_t scale)
{
    return jd_decomp(jd, outfunc, scale);
}