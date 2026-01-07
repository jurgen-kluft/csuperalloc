#include "ccore/c_target.h"
#include "ccore/c_debug.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"

#include "csuperalloc/private/c_list.h"

namespace ncore
{
    // 32 bit linked list

    llist32_t::llist32_t(u32* array_next, u32* array_prev)
        : m_array_next(array_next)
        , m_array_prev(array_prev)
    {
    }

    void llist32_t::add(u32& head, u32 index)
    {
        if (head == D_NILL_U32)
        {
            m_array_next[index] = index;
            m_array_prev[index] = index;
            head                = index;
        }
        else
        {
            u32 const tail      = m_array_prev[head];
            m_array_next[tail]  = index;
            m_array_prev[index] = tail;
            m_array_next[index] = head;
            m_array_prev[head]  = index;
        }
    }

    void llist32_t::rem(u32& head, u32 index)
    {
        if (m_array_next[index] == index)
        {
            ASSERT(head == index);
            head = D_NILL_U32;
        }
        else
        {
            u32 const next_index     = m_array_next[index];
            u32 const prev_index     = m_array_prev[index];
            m_array_next[prev_index] = next_index;
            m_array_prev[next_index] = prev_index;
        }
        m_array_next[index] = D_NILL_U32;
        m_array_prev[index] = D_NILL_U32;
    }

    u32 llist32_t::pop(u32& head)
    {
        u32 item = head;
        if (item != D_NILL_U32)
        {
            head                             = (m_array_next[item] == item) ? D_NILL_U32 : m_array_next[item];
            m_array_next[m_array_prev[item]] = m_array_next[item];
            m_array_prev[m_array_next[item]] = m_array_prev[item];
            m_array_prev[item]               = D_NILL_U32;
            m_array_next[item]               = D_NILL_U32;
        }
        return item;
    }

    // 16 bit linked list

    llist16_t::llist16_t(u16* array_next, u16* array_prev)
        : m_array_next(array_next)
        , m_array_prev(array_prev)
    {
    }

    void llist16_t::add(u16& head, u16 index)
    {
        if (head == D_NILL_U16)
        {
            m_array_next[index] = index;
            m_array_prev[index] = index;
            head                = index;
        }
        else
        {
            u16 const tail      = m_array_prev[head];
            m_array_next[tail]  = index;
            m_array_prev[index] = tail;
            m_array_next[index] = head;
            m_array_prev[head]  = index;
        }
    }

    void llist16_t::rem(u16& head, u16 index)
    {
        if (m_array_next[index] == index)
        {
            ASSERT(head == index);
            head = D_NILL_U16;
        }
        else
        {
            u16 const next_index     = m_array_next[index];
            u16 const prev_index     = m_array_prev[index];
            m_array_next[prev_index] = next_index;
            m_array_prev[next_index] = prev_index;
        }
        m_array_next[index] = D_NILL_U16;
        m_array_prev[index] = D_NILL_U16;
    }

    u16 llist16_t::pop(u16& head)
    {
        u16 item = head;
        if (item != D_NILL_U16)
        {
            head                             = (m_array_next[item] == item) ? D_NILL_U16 : m_array_next[item];
            m_array_next[m_array_prev[item]] = m_array_next[item];
            m_array_prev[m_array_next[item]] = m_array_prev[item];
            m_array_prev[item]               = D_NILL_U16;
            m_array_next[item]               = D_NILL_U16;
        }
        return item;
    }

}  // namespace ncore
