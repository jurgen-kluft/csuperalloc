#ifndef __CSUPERALLOC_BINMAP_H_
#define __CSUPERALLOC_BINMAP_H_
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

#include "ccore/c_debug.h"

namespace ncore
{
    struct binmap_t
    {
        void init(u32 count, u16* l1, u32 l1len, u16* l2, u32 l2len);
        void init1(u32 count, u16* l1, u32 l1len, u16* l2, u32 l2len);
        void set(u32 count, u16* l1, u16* l2, u32 bin);
        void clr(u32 count, u16* l1, u16* l2, u32 bin);
        bool get(u32 count, u16 const* l2, u32 bin) const;
        s32  find(u32 count, u16 const* l1, u16 const* l2) const;
        s32  findandset(u32 count, u16* l1, u16* l2);

        u32 m_l0;
        u32 m_l1_offset;
        u32 m_l2_offset;
    };

} // namespace ncore

#endif // __CSUPERALLOC_BINMAP_H_