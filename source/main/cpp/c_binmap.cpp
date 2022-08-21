#include "cbase/c_target.h"
#include "cbase/c_debug.h"
#include "cbase/c_allocator.h"
#include "cbase/c_memory.h"
#include "cbase/c_integer.h"

#include "xsuperalloc/private/x_binmap.h"

namespace ncore
{
    u32 resetarray(u32 count, u32 len, u16* data, u16 df = 0)
    {
        u32 const wi2 = count >> 4;
        for (u32 i = 0; i < wi2; i++)
            data[i] = df;

        u32 w = wi2;
        u32 const r = ((count&(16-1)) + (16-1)) >> 4;
        if (r == 1)
        {
            u16 const m = 0xffff << (count & (16 - 1));
            data[w++] = m | (df & ~m);
        }
        while (w < len)
            data[w++] = 0xffff;
        return wi2 + r;
    }

    void binmap_t::init(u32 count, u16* l1, u32 l1len, u16* l2, u32 l2len)
    {
        // Set those bits that we never touch to '1' the rest to '0'
        if (count > 32)
        {
            u32 const c2 = resetarray(count, l2len, l2);
            u32 const c1 = resetarray(c2, l1len, l1);
            count = c1;
        }
        if (count == 32)
            m_l0 = 0;
        else
            m_l0 = 0xffffffff << count;
    }

    void binmap_t::init1(u32 count, u16* l1, u32 l1len, u16* l2, u32 l2len)
    {
        // Set all bits to '1'
        if (count > 32)
        {
            u32 const c2 = resetarray(count, l2len, l2, 0xffff);
            u32 const c1 = resetarray(c2, l1len, l1, 0xffff);
            count = c1;
        }
        m_l0 = 0xffffffff;
    }

    void binmap_t::set(u32 count, u16* l1, u16* l2, u32 k)
    {
        if (count <= 32)
        {
            u32 const bi0 = 1 << (k & (32 - 1));
            u32 const wd0 = m_l0 | bi0;
            m_l0          = wd0;
        }
        else
        {
            u32 const wi2 = k / 16;
            u16 const bi2 = (u16)1 << (k & (16 - 1));
            u16 const wd2 = l2[wi2] | bi2;
            if (wd2 == 0xffff)
            {
                u32 const wi1 = wi2 / 16;
                u16 const bi1 = 1 << (wi2 & (16 - 1));
                u16 const wd1 = l1[wi1] | bi1;
                if (wd1 == 0xffff)
                {
                    u32 const bi0 = 1 << (wi1 & (32 - 1));
                    u32 const wd0 = m_l0 | bi0;
                    m_l0          = wd0;
                }
                l1[wi1] = wd1;
            }
            l2[wi2] = wd2;
        }
    }

    void binmap_t::clr(u32 count, u16* l1, u16* l2, u32 k)
    {
        if (count <= 32)
        {
            u32 const bi0 = 1 << (k & (32 - 1));
            u32 const wd0 = m_l0 & ~bi0;
            m_l0          = wd0;
        }
        else
        {
            u32 const wi2 = k / 16;
            u16 const bi2 = (u16)1 << (k & (16 - 1));
            u16 const wd2 = l2[wi2];
            if (wd2 == 0xffff)
            {
                u32 const wi1 = wi2 / 16;
                u16 const bi1 = 1 << (wi2 & (16 - 1));
                u16 const wd1 = l1[wi1];
                if (wd1 == 0xffff)
                {
                    u32 const bi0 = 1 << (wi1 & (32 - 1));
                    u32 const wd0 = m_l0 & ~bi0;
                    m_l0          = wd0;
                }
                l1[wi1] = wd1 & ~bi1;
            }
            l2[wi2] = wd2 & ~bi2;
        }
    }

    bool binmap_t::get(u32 count, u16 const* l2, u32 k) const
    {
        if (count <= 32)
        {
            u32 const bi0 = 1 << (k & (32 - 1));
            return (m_l0 & bi0) != 0;
        }
        else
        {
            u32 const wi2 = k / 16;
            u16 const bi2 = (u16)1 << (k & (16 - 1));
            u16 const wd2 = l2[wi2];
            return (wd2 & bi2) != 0;
        }
    }

    s32 binmap_t::find(u32 count, u16 const* l1, u16 const* l2) const
    {
        s32 const bi0 = xfindFirstBit(~m_l0);
        if (bi0 >= 0 && count > 32)
        {
            u32 const wi1 = bi0 * 16;
            s32 const bi1 = xfindFirstBit((u16)~l1[wi1]);
            ASSERT(bi1 >= 0);
            u32 const wi2 = wi1 * 16 + bi1;
            s32 const bi2 = xfindFirstBit((u16)~l2[wi2]);
            ASSERT(bi2 >= 0);
            return bi2 + wi2 * 16;
        }
        return bi0;
    }

    s32 binmap_t::findandset(u32 count, u16* l1, u16* l2)
    {
        s32 const bi0 = xfindFirstBit(~m_l0);
        if (bi0 < 0)
            return -1;

        if (count <= 32)
        {
            u32 const bd0 = 1 << (bi0 & (32 - 1));
            u32 const wd0 = m_l0 | bd0;
            m_l0          = wd0;
            return bi0;
        }
        else
        {
            u32 const wi1 = bi0;
            s32 const bi1 = xfindFirstBit((u16)~l1[wi1]);
            ASSERT(bi1 >= 0);
            u32 const wi2 = (wi1 * 16) + bi1;
            s32 const bi2 = xfindFirstBit((u16)~l2[wi2]);
            ASSERT(bi2 >= 0);
            u32 const k   = (wi2 * 16) + bi2;
            ASSERT(get(count, l2, k) == false);

            u16 const wd2 = l2[wi2] | (1 << bi2);
            if (wd2 == 0xffff)
            {
                u16 const wd1 = l1[wi1] | (1 << bi1);
                if (wd1 == 0xffff)
                {
                    u32 const b0  = 1 << (wi1 & (32 - 1));
                    u32 const wd0 = m_l0 | b0;
                    m_l0          = wd0;
                }
                l1[wi1] = wd1;
            }
            l2[wi2] = wd2;
            return k;
        }
    }

} // namespace ncore
