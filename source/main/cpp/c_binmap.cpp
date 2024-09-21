#include "ccore/c_target.h"
#include "ccore/c_debug.h"
#include "cbase/c_allocator.h"
#include "cbase/c_memory.h"
#include "cbase/c_integer.h"

#include "csuperalloc/private/c_binmap.h"

namespace ncore
{
    // l0 :  32 =  1  * u32
    // l1 :  1K =  32 * u32
    // l2 : 32K =  1K * u32
    // l3 :  1M = 32K * u32

    s8 binmap_t::compute_levels(u32 count, u32& l0, u32& l1, u32& l2, u32& l3)
    {
        l1 = l2 = l3 = 0;
        ASSERT(count > 0 && count <= 1 * 1024 * 1024);                // maximum count is 1 Million (5 bits + 5 bits + 5 bits + 5 bits = 20 bits = 1 M)
        s8 const levels = (math::mostSignificantBit(count - 1) / 5);  // We can have a maximum of 4 levels, each level holds 5 bits
        u32      len    = count;
        switch (levels)
        {
            case 3: l3 = len; len = (len + 31) >> 5;  // fall through
            case 2: l2 = len; len = (len + 31) >> 5;  // fall through
            case 1: l1 = len; len = (len + 31) >> 5;  // fall through
        }
        l0 = len;
        return levels;
    }

    u32 binmap_t::sizeof_data(u32 count)
    {
        u32 l0, l1, l2, l3;
        compute_levels(count, l0, l1, l2, l3);
        return sizeof(binmap_t) + ((l1 + l2 + l3) >> 3);
    }

    static void resetarray_full(u32 level_bits, u32* level, u32 df)
    {
        u32 const w = level_bits >> 5;
        for (u32 i = 0; i < w; i++)
            level[i] = df;
        if ((level_bits & (32 - 1)) != 0)
        {
            u32 const m = 0xffffffff << (level_bits & (32 - 1));
            level[w]    = m | (df & ~m);
        }
    }

    void binmap_t::init_all_free()
    {
        u32 const count = size();
        u32       l0, l1, l2, l3;
        compute_levels(count, l0, l1, l2, l3);
        init_all_free(count, l0, m_l[0], l1, m_l[1], l2, m_l[2], l3);
    }

    void binmap_t::init_all_used()
    {
        u32 const count = size();
        u32       l0, l1, l2, l3;
        compute_levels(count, l0, l1, l2, l3);
        init_all_used(count, l0, m_l[0], l1, m_l[1], l2, m_l[2], l3);
    }

    void binmap_t::init_all_free(u32 count, alloc_t* allocator)
    {
        u32 l0, l1, l2, l3;
        compute_levels(count, l0, l1, l2, l3);
        u32* d1 = (l1 > 0) ? (u32*)allocator->allocate(sizeof(u32) * l1) : nullptr;
        u32* d2 = (l2 > 0) ? (u32*)allocator->allocate(sizeof(u32) * l2) : nullptr;
        u32* d3 = (l3 > 0) ? (u32*)allocator->allocate(sizeof(u32) * l3) : nullptr;
        init_all_free(count, l0, d1, l1, d2, l2, d3, l3);
    }

    void binmap_t::init_all_used(u32 count, alloc_t* allocator)
    {
        u32 l0, l1, l2, l3;
        compute_levels(count, l0, l1, l2, l3);
        u32* d1 = (l1 > 0) ? (u32*)allocator->allocate(sizeof(u32) * l1) : nullptr;
        u32* d2 = (l2 > 0) ? (u32*)allocator->allocate(sizeof(u32) * l2) : nullptr;
        u32* d3 = (l3 > 0) ? (u32*)allocator->allocate(sizeof(u32) * l3) : nullptr;
        init_all_used(count, l0, d1, l1, d2, l2, d3, l3);
    }

    void binmap_t::release(alloc_t* allocator)
    {
        switch (levels())
        {
            case 3: allocator->deallocate(m_l[2]);
            case 2: allocator->deallocate(m_l[1]);
            case 1: allocator->deallocate(m_l[0]);
        }
        reset();
    }

    void binmap_t::init_all_free_lazy(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len)
    {
        ASSERT((l3len == 0 || l3 != nullptr) && (l2len == 0 || l2 != nullptr) && (l1len == 0 || l1 != nullptr) && (l0len > 0));

        m_l[2] = l3;
        m_l[1] = l2;
        m_l[0] = l1;
        m_l0   = 0xffffffff;

        u32 const levels = (l3len > 0) ? 3 : (l2len > 0) ? 2 : ((l1len > 0) ? 1 : 0);
        m_count          = (levels << 28) | count;
    }

    void binmap_t::init_all_used_lazy(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len)
    {
        ASSERT((l3len == 0 || l3 != nullptr) && (l2len == 0 || l2 != nullptr) && (l1len == 0 || l1 != nullptr) && (l0len > 0));

        m_l[2] = l3;
        m_l[1] = l2;
        m_l[0] = l1;
        m_l0   = 0xffffffff;

        u32 const levels = (l3len > 0) ? 3 : (l2len > 0) ? 2 : ((l1len > 0) ? 1 : 0);
        m_count          = (levels << 28) | count;
    }

    void binmap_t::init_all_free(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len)
    {
        ASSERT((l3len == 0 || l3 != nullptr) && (l2len == 0 || l2 != nullptr) && (l1len == 0 || l1 != nullptr) && (l0len > 0));

        u32 const levels = (l3len > 0) ? 3 : (l2len > 0) ? 2 : ((l1len > 0) ? 1 : 0);
        m_count          = (levels << 28) | count;

        m_l0   = (u32)((u64)0xffffffff << l0len);
        m_l[0] = nullptr;
        m_l[1] = nullptr;
        m_l[2] = nullptr;

        switch (levels)
        {
            case 3: m_l[2] = l3; resetarray_full(l3len, l3, 0);
            case 2: m_l[1] = l2; resetarray_full(l2len, l2, 0);
            case 1: m_l[0] = l1; resetarray_full(l1len, l1, 0);
        }
    }

    void binmap_t::init_all_used(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len)
    {
        ASSERT((l3len == 0 || l3 != nullptr) && (l2len == 0 || l2 != nullptr) && (l1len == 0 || l1 != nullptr) && (l0len > 0));

        u32 const levels = (l3len > 0) ? 3 : (l2len > 0) ? 2 : ((l1len > 0) ? 1 : 0);
        m_count          = (levels << 28) | count;

        m_l0   = 0xffffffff;
        m_l[0] = nullptr;
        m_l[1] = nullptr;
        m_l[2] = nullptr;

        switch (levels)
        {
            case 3: m_l[2] = l3; resetarray_full(l3len, l3, 0xffffffff);
            case 2: m_l[1] = l2; resetarray_full(l2len, l2, 0xffffffff);
            case 1: m_l[0] = l1; resetarray_full(l1len, l1, 0xffffffff);
        }
    }

    // Note: We are tracking 'empty' place where we can set a bit
    void binmap_t::set_used(u32 bit)
    {
        u32 wi = bit;
        for (s8 l = levels() - 1; l >= 0; --l)
        {
            u32 const bi = (u32)1 << (wi & (32 - 1));
            wi           = wi >> 5;
            u32 wd       = m_l[l][wi];
            wd |= bi;
            m_l[l][wi] = wd;
            // If all bits are not set yet -> early out
            // Which means there are some empty places
            if (wd != 0xffffffff)
                return;
        }
        m_l0 = m_l0 | (1 << (wi & (32 - 1)));
    }

    void binmap_t::set_free(u32 bit)
    {
        u32 wi = bit;
        for (s8 l = levels() - 1; l >= 0; --l)
        {
            u32 const bi = (u32)1 << (wi & (32 - 1));
            wi           = wi >> 5;
            const u32 wd = m_l[l][wi];
            m_l[l][wi]   = wd & ~bi;
            // There are already where some empty places -> early out
            if (wd != 0xFFFFFFFF)
                return;
        }
        m_l0 = m_l0 & ~(1 << (wi & (32 - 1)));
    }

    bool binmap_t::get(u32 bit) const
    {
        u32 const l  = levels();
        u32 const bi = (u32)1 << (bit & (32 - 1));
        if (l == 0)
            return (m_l0 & bi) != 0;
        u32 const wi = bit >> 5;
        u32 const wd = m_l[l - 1][wi];
        return (wd & bi) != 0;
    }

    s32 binmap_t::find() const
    {
        if (m_l0 == 0xffffffff)
            return -1;

        u32 const l  = levels();
        u32       wi = 0;
        s8        bi = math::findFirstBit(~m_l0);
        ASSERT(bi >= 0 && bi < 32);
        for (u32 i = 0; i < l; ++i)
        {
            wi = (wi << 5) + bi;
            bi = math::findFirstBit(~m_l[i][wi]);
            ASSERT(bi >= 0 && bi < 32);
        }

        s32 const bin = (wi << 5) + bi;
        return (bin < size()) ? bin : -1;
    }

    s32 binmap_t::find_and_set()
    {
        s32 const bi = find();
        if (bi >= 0)
            set_used(bi);
        return bi;
    }

    s32 binmap_t::upper(u32 pivot)
    {
        if ((pivot + 1) >= size())
            return -1;

        // Start at bottom level and move up finding an empty place
        u32 iw = (pivot / 32);  // The index of a 32-bit word in bottom level
        u32 ib = (pivot & 31);  // The bit number in that 32-bit word
        u32 im;

        ib += 1;  // Search next bit and beyond
        if (ib == 32)
        {
            iw += 1;
            ib = 0;
            im = 0xffffffff;
        }
        else
        {
            im = ~((1 << ib) - 1);
        }

        s8 const ml = levels();
        s8       il = ml;

        while (il >= 0 && il <= ml)
        {
            u32 const* level = il == 0 ? &m_l0 : m_l[il - 1];
            u32 const  w     = (~level[iw]) & im;
            if (w != 0)
            {
                iw = (iw * 32) + math::findFirstBit(w);
                if (il == ml)
                    return iw;
                // Go down
                il += 1;
                ib = 0;
                im = 0xffffffff;
            }
            else
            {
                // Go up (move one unit in the direction of upper)
                il -= 1;
                ib = (iw & 31);
                iw = (iw / 32);
                ib += 1;
                if (ib == 32)
                {
                    iw += 1;
                    ib = 0;
                }
                im = ~((1 << ib) - 1);
            }
        };
        return -1;
    }

    s32 binmap_t::lower(u32 pivot)
    {
        if (pivot >= size())
            return -1;

        // Start at bottom level and move up finding a 'set' bit
        u32 iw = (pivot / 32);  // The index of a 32-bit word in level 0
        u32 ib = (pivot & 31);  // The bit number in that 32-bit word
        u32 im;
        if (ib == 0)
        {
            iw -= 1;
            im = 0xffffffff;
        }
        else
        {
            im = ((1 << ib) - 1);
        }

        s8 const ml = levels();
        s8       il = ml;

        while (il >= 0 && il <= ml)
        {
            u32 const* level = il == 0 ? &m_l0 : m_l[il - 1];
            u32 const  w     = (~level[iw]) & im;
            if (w != 0)
            {
                iw = (iw * 32) + (u32)math::findFirstBit(w);
                if (il == ml)
                    return iw;

                // Go down
                il += 1;
                im = 0xffffffff;
            }
            else
            {
                // Go up (move one unit in the direction of lower)
                il -= 1;
                ib = (iw & 31);
                iw = (iw / 32);
                if (ib == 0)
                {
                    iw -= 1;
                    im = 0xffffffff;
                }
                else
                {
                    im = ((1 << ib) - 1);
                }
            }
        };
        return -1;
    }

    void binmap_t::init_all_free_lazy(u32 bit)
    {
        u32 wi = bit;
        for (s8 l = levels() - 1; l >= 0; --l)
        {
            const u32 li = wi & (32 - 1);
            wi           = wi >> 5;
            const u32 wd = (li == 0) ? 0xffffffff : m_l[l][wi];
            const u32 bi = (u32)1 << li;
            m_l[l][wi]   = wd & ~bi;
            if (wd != 0xffffffff)
                return;
        }
        m_l0 = m_l0 & ~((u32)1 << (wi & (32 - 1)));
    }

    void binmap_t::init_all_used_lazy(u32 bit)
    {
        u32 wi = bit;
        for (s8 l = levels() - 1; l >= 0; --l)
        {
            const u32 li = wi & (32 - 1);
            wi           = wi >> 5;
            u32 wd       = li == 0 ? 0xfffffffe : m_l[l][wi];
            wd |= (u32)1 << li;
            m_l[l][wi] = wd;
            if (wd != 0xffffffff)
                return;
        }
        m_l0 = m_l0 | ((u32)1 << (wi & (32 - 1)));
    }

    void binmap_t::iter_t::begin()
    {
        // Find the first free bit, starting from 'start'
        if (!m_bm->is_free(m_cur))
        {
            s32 const start = m_bm->upper(m_cur);
            if (start >= 0)
            {
                m_cur = start;
            }
            else
            {
                m_cur = m_end;
            }
        }
    }

    void binmap_t::iter_t::next()
    {
        m_cur = m_bm->upper(m_cur);
        if (m_cur < 0)
            m_cur = m_end;
    }

}  // namespace ncore
