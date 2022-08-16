#include "xbase/x_target.h"
#include "xbase/x_debug.h"
#include "xbase/x_allocator.h"
#include "xbase/x_integer.h"

#include "xsuperalloc/private/x_doubly_linked_list.h"

namespace xcore
{
    void llist_t::initialize(lldata_t& data, u16 start, u16 size, u16 max_size)
    {
        ASSERT(max_size > 0);
        ASSERT(size <= max_size);
        m_size         = size;
        m_size_max     = max_size;
        m_head.m_index = start;

        const u32 end = start + size;
        for (u32 i = 0; i < size; ++i)
        {
            u32 const t    = start + i;
            llnode_t* node = idx2node(data, t);
            node->m_prev = (t - 1);
            node->m_next = (t + 1);
        }
        llnode_t* snode = idx2node(data, start);
        snode->m_prev = end - 1;
        snode->m_next = start + 1;
        llnode_t* enode = idx2node(data, end - 1);
        enode->m_prev = end - 2;
        enode->m_next = start;

    }

    void llhead_t::insert(lldata_t& data, llindex_t item)
    {
        llnode_t* const pitem = data.idx2node(item);
        if (is_nil())
        {
            pitem->m_prev = item;
            pitem->m_next = item;
        }
        else
        {
            llindex_t const inext = m_index;
            llnode_t* const pnext = data.idx2node(inext);
            llindex_t const iprev = pnext->m_prev;
            llnode_t* const pprev = data.idx2node(iprev);
            pitem->m_prev = iprev;
            pitem->m_next = inext;
            pnext->m_prev = item;
            pprev->m_next = item;
        }
        m_index = item;
    }

    void llhead_t::insert_tail(lldata_t& data, llindex_t item)
    {
        llnode_t* const pitem = data.idx2node(item);
        if (is_nil())
        {
            pitem->m_prev = item;
            pitem->m_next = item;
            m_index = item;
        }
        else
        {
            llindex_t const inext = m_index;
            llnode_t* const pnext = data.idx2node(inext);
            llindex_t const iprev = pnext->m_prev;
            llnode_t* const pprev = data.idx2node(iprev);
            pitem->m_prev = iprev;
            pitem->m_next = inext;
            pnext->m_prev = item;
            pprev->m_next = item;
        }
    }

    static void s_remove_item(llhead_t& head, lldata_t& data, llindex_t item, llnode_t*& out_node)
    {
        llnode_t* const pitem = data.idx2node(item);
        llnode_t* const phead = data.idx2node(head.m_index);
        if (phead->m_prev == head.m_index && phead->m_next == head.m_index)
        {
            ASSERT(head.m_index == item);
            head.reset();
        }
        else
        {
            llnode_t* const pprev = data.idx2node(pitem->m_prev);
            llnode_t* const pnext = data.idx2node(pitem->m_next);
            pprev->m_next         = pitem->m_next;
            pnext->m_prev         = pitem->m_prev;
            if (item == head.m_index)
                head.m_index = pprev->m_next;
        }
        pitem->m_prev = llnode_t::NIL;
        pitem->m_next = llnode_t::NIL;
        out_node      = pitem;
    }

    static s32 s_remove_tail(llhead_t& head, lldata_t& data, llnode_t*& out_node)
    {
        if (head.is_nil())
            return 0;
        llnode_t* const phead = data.idx2node(head.m_index);
        llindex_t tail = phead->m_prev;
        s_remove_item(head, data, tail, out_node);
        return 1;
    }

    llnode_t* llhead_t::remove_item(lldata_t& data, llindex_t item)
    {
        llnode_t* node = nullptr;
        if (!is_nil())
            s_remove_item(*this, data, item, node);
        return node;
    }

    llnode_t* llhead_t::remove_head(lldata_t& data)
    {
        llnode_t* node = nullptr;
        if (!is_nil())
            s_remove_item(*this, data, m_index, node);
        return node;
    }

    llnode_t* llhead_t::remove_tail(lldata_t& data)
    {
        llnode_t* node = nullptr;
        if (!is_nil())
            s_remove_tail(*this, data, node);
        return node;
    }

    llindex_t llhead_t::remove_headi(lldata_t& data)
    {
        llindex_t item = m_index;
        if (item != llnode_t::NIL)
        {
            llnode_t* node = nullptr;
            s_remove_item(*this, data, m_index, node);
        }
        return item;
    }

    llindex_t llhead_t::remove_taili(lldata_t& data)
    {
        if (is_nil())
            return m_index;
        llnode_t* node;
        s_remove_tail(*this, data, node);
        return data.node2idx(node);
    }

    void llist_t::insert(lldata_t& data, llindex_t item)
    {
        ASSERT(m_size < m_size_max);
        m_head.insert(data, item);
        m_size += 1;
    }

    void llist_t::insert_tail(lldata_t& data, llindex_t item)
    {
        ASSERT(m_size < m_size_max);
        m_head.insert_tail(data, item);
        m_size += 1;
    }

    llnode_t* llist_t::remove_item(lldata_t& data, llindex_t item)
    {
        llnode_t* node = nullptr;
        ASSERT(m_size > 0);
        m_size -= s_remove_item(m_head, data, item, node);
        return node;
    }

    llnode_t* llist_t::remove_head(lldata_t& data)
    {
        llnode_t* node = nullptr;
        ASSERT(m_size > 0);
        llindex_t item = m_head.m_index;
        m_size -= s_remove_item(m_head, data, item, node);
        return node;
    }

    llnode_t* llist_t::remove_tail(lldata_t& data)
    {
        llnode_t* node = nullptr;
        ASSERT(m_size > 0);
        m_size -= s_remove_tail(m_head, data, node);
        return node;
    }

    llindex_t llist_t::remove_headi(lldata_t& data)
    {
        llnode_t* node = nullptr;
        ASSERT(m_size > 0);
        llindex_t item = m_head.m_index;
        s_remove_item(m_head, data, item, node);
        m_size -= 1;
        return item;
    }

    llindex_t llist_t::remove_taili(lldata_t& data)
    {
        llnode_t* node = nullptr;
        ASSERT(m_size > 0);
        m_size -= s_remove_tail(m_head, data, node);
        return node2idx(data, node);
    }

} // namespace xcore