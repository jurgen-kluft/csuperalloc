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

    // TODO, We could encode the level 'bits left' into m_count
    // e.g.:
    //       l0 = 2     (30 '0' bits, encode in 0x0000001F)
    //       l1 = 60    ( 4 '0' bits, encode in 0x000003E0)
    //       l2 = 1900  (20 '0' bits, encode in 0x00007C00)
    //       l3 = 60790 (10 '0' bits, encode in 0x000F8000)

    // You can then always compute the full count = ((32 - [l0]) * 32 - [l1]) * 32 - [l2]) * 32 - [l3]

    u32 binmap_t::compute_levels(u32 count, u32& l0, u32& l1, u32& l2, u32& l3)
    {
        ASSERT(count <= 1 * 1024 * 1024);  // maximum count is 1 Million (5 bits + 5 bits + 5 bits + 5 bits = 20 bits = 1 M)
        l0 = l1 = l2 = l3 = 0;
        if (count == 0)
            return 0;

        // We can have a maximum of 4 levels, each level holds 5 bits
        u32 const levels = ((count - 1) & 0x000F8000) ? 4 : (((count - 1) & 0x00007C00) ? 3 : (((count - 1) & 0x000003E0) ? 2 : 1));

        u32 len = count;
        switch (levels)
        {
            case 4: l3 = len; len = (len + 31) >> 5;  // fall through
            case 3: l2 = len; len = (len + 31) >> 5;  // fall through
            case 2: l1 = len; len = (len + 31) >> 5;  // fall through
            case 1: l0 = len; break;
        }
        return levels - 1;
    }

    static void resetarray_end(u32 level_bits, u32* level, u32 df)
    {
        if ((level_bits & (32 - 1)) != 0)
        {
            u32 const w = level_bits >> 5;
            u32 const m = 0xffffffff << (level_bits & (32 - 1));
            level[w]    = m | (df & ~m);
        }
        else
        {
            u32 const w = (level_bits >> 5) - 1;
            level[w]    = df;
        }
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

    void binmap_t::init_lazy_0(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len)
    {
        m_l[2] = l3;
        m_l[1] = l2;
        m_l[0] = l1;
        m_l0   = 0xffffffff;

        if (l3 != nullptr && l3len > 0)
        {
            m_count = (3 << 30) | count;
        }
        else if (l2 != nullptr && l2len > 0)
        {
            m_count = (2 << 30) | count;
        }
        else if (l1 != nullptr && l1len > 0)
        {
            m_count = (1 << 30) | count;
        }
        else
        {
            m_count = (0 << 30) | count;
        }
    }

    void binmap_t::init_lazy_1(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len)
    {
        m_l[2] = l3;
        m_l[1] = l2;
        m_l[0] = l1;

        if (l3 != nullptr && l3len > 0)
        {
            m_count = (3 << 30) | count;
            resetarray_end(l3len, l3, 0xffffffff);
            resetarray_end(l2len, l2, 0xffffffff);
            resetarray_end(l1len, l1, 0xffffffff);
        }
        else if (l2 != nullptr && l2len > 0)
        {
            m_count = (2 << 30) | count;
            resetarray_end(l2len, l2, 0xffffffff);
            resetarray_end(l1len, l1, 0xffffffff);
        }
        else if (l1 != nullptr && l1len > 0)
        {
            m_count = (1 << 30) | count;
            resetarray_end(l1len, l1, 0xffffffff);
        }
        else
        {
            m_count = (0 << 30) | count;
        }
        m_l0 = 0xffffffff;
    }

    void binmap_t::init_0(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len)
    {
        m_l[2] = l3;
        m_l[1] = l2;
        m_l[0] = l1;

        if (l3 != nullptr && l3len > 0)
        {
            m_count = (3 << 30) | count;
            resetarray_full(l3len, l3, 0);
            resetarray_full(l2len, l2, 0);
            resetarray_full(l1len, l1, 0);
        }
        else if (l2 != nullptr && l2len > 0)
        {
            m_count = (2 << 30) | count;
            resetarray_full(l2len, l2, 0);
            resetarray_full(l1len, l1, 0);
        }
        else if (l1 != nullptr && l1len > 0)
        {
            m_count = (1 << 30) | count;
            resetarray_full(l1len, l1, 0);
        }
        else
        {
            m_count = (0 << 30) | count;
        }

        switch (l0len)
        {
            case 32: m_l0 = 0; break;
            default: m_l0 = 0xffffffff << l0len; break;
        }
    }

    void binmap_t::init_1(u32 count, u32 l0len, u32* l1, u32 l1len, u32* l2, u32 l2len, u32* l3, u32 l3len)
    {
        m_l[2] = l3;
        m_l[1] = l2;
        m_l[0] = l1;

        // Set all bits to '1'
        if (l3 != nullptr && l3len > 0)
        {
            m_count = (3 << 30) | count;
            resetarray_full(l3len, l3, 0xffffffff);
            resetarray_full(l2len, l2, 0xffffffff);
            resetarray_full(l1len, l1, 0xffffffff);
        }
        else if (l2 != nullptr && l2len > 0)
        {
            m_count = (2 << 30) | count;
            resetarray_full(l2len, l2, 0xffffffff);
            resetarray_full(l1len, l1, 0xffffffff);
        }
        else if (l1 != nullptr && l1len > 0)
        {
            m_count = (1 << 30) | count;
            resetarray_full(l1len, l1, 0xffffffff);
        }
        else
        {
            m_count = (0 << 30) | count;
        }
        m_l0 = 0xffffffff;
    }

    void binmap_t::set(u32 bit)
    {
        u32 bi;
        u32 wi = bit;
        u32 wd;

        switch (num_levels())
        {
            case 0: goto level_0;
            case 1: goto level_1;
            case 2: goto level_2;
            case 3: goto level_3;
        }

    level_3:
        bi = (u32)1 << (wi & (32 - 1));
        wi = wi >> 5;
        wd = m_l[2][wi];
        if (wd == 0xffffffff)
            return;
        wd |= bi;
        m_l[2][wi] = wd;
        if (wd != 0xffffffff)
            return;

    level_2:
        bi = (u32)1 << (wi & (32 - 1));
        wi = wi >> 5;
        wd = m_l[1][wi];
        if (wd == 0xffffffff)
            return;
        wd |= bi;
        m_l[1][wi] = wd;
        if (wd != 0xffffffff)
            return;

    level_1:
        bi = 1 << (wi & (32 - 1));
        wi = wi >> 5;
        wd = m_l[0][wi];
        if (wd == 0xffffffff)
            return;
        wd |= bi;
        m_l[0][wi] = wd;
        if (wd != 0xffffffff)
            return;

    level_0:
        bi   = 1 << (wi & (32 - 1));
        wd   = m_l0 | bi;
        m_l0 = wd;
    }

    void binmap_t::clr(u32 bit)
    {
        u32 bi;
        u32 wi = bit;
        u32 wd;

        switch (num_levels())
        {
            case 0: goto level_0;
            case 1: goto level_1;
            case 2: goto level_2;
        }

        // level_3:
        bi         = (u32)1 << (wi & (32 - 1));
        wi         = wi << 5;
        wd         = m_l[2][wi];
        m_l[2][wi] = wd & ~bi;
        if (wd != 0xffffffff)
            return;

    level_2:
        bi         = (u32)1 << (wi & (32 - 1));
        wi         = wi << 5;
        wd         = m_l[1][wi];
        m_l[1][wi] = wd & ~bi;
        if (wd != 0xffffffff)
            return;

    level_1:
        bi         = (u32)1 << (wi & (32 - 1));
        wi         = wi << 5;
        wd         = m_l[0][wi];
        m_l[0][wi] = wd & ~bi;
        if (wd != 0xffffffff)
            return;

    level_0:
        bi   = 1 << (wi & (32 - 1));
        wd   = m_l0 & ~bi;
        m_l0 = wd;
    }

    bool binmap_t::get(u32 bit) const
    {
        u32 const l = num_levels();
        if (l == 0)
        {
            u32 const bi0 = 1 << (bit & (32 - 1));
            return (m_l0 & bi0) != 0;
        }
        else
        {
            u32 const wi2 = bit >> 5;
            u32 const bi2 = (u32)1 << (bit & (32 - 1));
            u32 const wd2 = m_l[l - 1][wi2];
            return (wd2 & bi2) != 0;
        }
    }

    s32 binmap_t::find() const
    {
        if (m_l0 == 0xffffffff)
            return -1;

        u32 l = num_levels();

        u32 wi;
        u32 bi = math::findFirstBit(~m_l0);
        if (l == 0)
            return bi;

        for (s32 i=0; i<l; ++l)
        {
            wi = bi;
            bi = math::findFirstBit(~m_l[i][wi]);
        }
        return (wi << 5) + bi;

#if 0
        wi = bi;
        bi = math::findFirstBit(~m_l[0][wi]);
        ASSERT(bi >= 0 && bi < 32);
        if (l == 1)
            return (wi << 5) + bi;

        wi = (wi << 5) + bi;
        bi = math::findFirstBit(~m_l[1][wi]);
        ASSERT(bi >= 0 && bi < 32);
        if (l == 2)
            return (wi << 5) + bi;

        ASSERT(l == 3);
        wi = (wi << 5) + bi;
        bi = math::findFirstBit(~m_l[2][wi]);
        ASSERT(bi >= 0 && bi < 32);
        return (wi << 5) + bi;
#endif
    }

    s32 binmap_t::findandset()
    {
        // TODO: This is not efficient, we should be able to do this inline here without using find() + set()
        s32 const bi = find();
        if (bi >= 0)
            set(bi);
        return bi;
    }

    void binmap_t::lazy_init_0(u32 bit)
    {
        u32 bi;
        u32 li;
        u32 wi = bit;
        u32 wd;

        for (s32 l = num_levels() - 1; l >= 0; --l)
        {
            li         = wi & (32 - 1);
            wi         = wi >> 5;
            wd         = (li == 0) ? 0xffffffff : m_l[l][wi];
            bi         = (u32)1 << li;
            m_l[l][wi] = wd & ~bi;
            if (wd != 0xffffffff)
                return;
        }
        li   = wi & (32 - 1);
        m_l0 = m_l0 & ~((u32)1 << li);
        return;

#if 0
        switch (num_levels())
        {
            case 0: goto level_0;
            case 1: goto level_1;
            case 2: goto level_2;
            case 3: goto level_3;
        }

    level_3:
        li         = wi & (32 - 1);
        wi         = wi << 5;
        wd         = (li == 0) ? 0xffffffff : m_l[2][wi];
        bi         = (u32)1 << li;
        m_l[2][wi] = wd & ~bi;
        if (wd != 0xffffffff)
            return;

    level_2:
        li         = wi & (32 - 1);
        wi         = wi << 5;
        wd         = (li == 0) ? 0xffffffff : m_l[1][wi];
        bi         = (u32)1 << li;
        m_l[1][wi] = wd & ~bi;
        if (wd != 0xffffffff)
            return;

    level_1:
        li         = wi & (32 - 1);
        wi         = wi << 5;
        wd         = (li == 0) ? 0xffffffff : m_l[0][wi];
        bi         = (u32)1 << li;
        m_l[0][wi] = wd & ~bi;
        if (wd != 0xffffffff)
            return;

    level_0:
        li   = wi & (32 - 1);
        m_l0 = m_l0 & ~((u32)1 << li);
#endif
    }

    void binmap_t::lazy_init_1(u32 bit)
    {
        u32 wi = bit;
        u32 li;
        u32 wd;

        for (s32 l = num_levels() - 1; l > 0; --l)
        {
            li = wi & (32 - 1);
            wi = wi >> 5;
            wd = li == 0 ? 0xfffffffe : m_l[l][wi];
            if (wd == 0xffffffff)
                return;
            wd |= (u32)1 << li;
            m_l[l][wi] = wd;
            if (wd != 0xffffffff)
                return;
        }
        li   = wi & (32 - 1);
        m_l0 = m_l0 | ((u32)1 << li);
        return;

#if 0
        switch (num_levels())
        {
            case 0: goto level_0;
            case 1: goto level_1;
            case 2: goto level_2;
            case 3: goto level_3;
        }

    level_3:
        li = wi & (32 - 1);
        wi = wi >> 5;
        wd = li == 0 ? 0xfffffffe : m_l[2][wi];
        if (wd == 0xffffffff)
            return;
        wd |= (u32)1 << li;
        m_l[2][wi] = wd;
        if (wd != 0xffffffff)
            return;

    level_2:
        li = wi & (32 - 1);
        wi = wi >> 5;
        wd = li == 0 ? 0xfffffffe : m_l[1][wi];
        if (wd == 0xffffffff)
            return;
        wd |= (u32)1 << li;
        m_l[1][wi] = wd;
        if (wd != 0xffffffff)
            return;

    level_1:
        li = wi & (32 - 1);
        wi = wi >> 5;
        wd = li == 0 ? 0xfffffffe : m_l[0][wi];
        if (wd == 0xffffffff)
            return;
        wd |= (u32)1 << li;
        m_l[0][wi] = wd;
        if (wd != 0xffffffff)
            return;

    level_0:
        li   = wi & (32 - 1);
        m_l0 = m_l0 | ((u32)1 << li);
#endif
    }

}  // namespace ncore
