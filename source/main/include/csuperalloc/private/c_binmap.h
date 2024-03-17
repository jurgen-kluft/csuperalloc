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
        // This will output the number of bits in each level and return the number of levels
        static u32 compute_levels(u32 count, u32& l0, u32& l1, u32& l2, u32& l3);

        void init_0(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len);     // Clear the levels with 0
        void init_lazy_0(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len);  // Do not not clear the levels, only the ends
        void lazy_init_0(u32 bit);
        void init_1(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len);  // Clear the levels with 1
        void init_lazy_1(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len);  // Do not not clear the levels, only the ends
        void lazy_init_1(u32 bit); // Progressive lazy initialization
        void set(u32 bit);
        void clr(u32 bit);
        bool get(u32 bit) const;
        s32  find() const;
        s32  findandset();

        // The (lowest level) index of the branch to initialize

        // m_count -> 0xFF000000 = number of levels, 0x00FFFFFF = number of bits
        inline s32 size() const { return m_count & 0x00FFFFFF; }
        inline s32 num_levels() const { return m_count >> 24; }

        u32  m_count;  // 0xFF000000 = number of levels, 0x00FFFFFF = number of bits
        u32  m_l0;     // Level 0 is 32 bits
        u32* m_l[3];   // Separate the allocation of level data (better allocation sizes)
    };

}  // namespace ncore

#endif  // __CSUPERALLOC_BINMAP_H_