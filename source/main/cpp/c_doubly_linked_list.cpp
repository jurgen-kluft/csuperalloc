#include "ccore/c_target.h"
#include "ccore/c_debug.h"
#include "cbase/c_allocator.h"
#include "cbase/c_integer.h"

#include "csuperalloc/private/c_doubly_linked_list.h"

namespace ncore
{
    void ll_insert_tail(llindex_t& head, dexer_t* dexer, llindex_t item)
    {
        llnode_t* const pitem = dexer->idx2obj<llnode_t>(item);
        if (head == llnode_t::NIL)
        {
            pitem->m_prev = item;
            pitem->m_next = item;
            head          = item;
        }
        else
        {
            llindex_t const inext = head;
            llnode_t* const pnext = dexer->idx2obj<llnode_t>(inext);
            llindex_t const iprev = pnext->m_prev;
            llnode_t* const pprev = dexer->idx2obj<llnode_t>(iprev);
            pitem->m_prev         = iprev;
            pitem->m_next         = inext;
            pnext->m_prev         = item;
            pprev->m_next         = item;
        }
    }

    void ll_insert(llindex_t& head, dexer_t* dexer, llindex_t item)
    {
        ll_insert_tail(head, dexer, item);
        head = item;
    }

    static void s_remove_item(llindex_t& head, dexer_t* dexer, llindex_t item, llnode_t*& out_node)
    {
        llnode_t* const pitem = dexer->idx2obj<llnode_t>(item);
        llnode_t* const phead = dexer->idx2obj<llnode_t>(head);
        if (phead->m_prev == head && phead->m_next == head)
        {
            ASSERT(head == item);
            head = llnode_t::NIL;
        }
        else
        {
            llnode_t* const pprev = dexer->idx2obj<llnode_t>(pitem->m_prev);
            llnode_t* const pnext = dexer->idx2obj<llnode_t>(pitem->m_next);
            pprev->m_next         = pitem->m_next;
            pnext->m_prev         = pitem->m_prev;
            if (item == head)
                head = pprev->m_next;
        }
        pitem->m_prev = llnode_t::NIL;
        pitem->m_next = llnode_t::NIL;
        out_node      = pitem;
    }

    static s32 s_remove_tail(llindex_t& head, dexer_t* dexer, llnode_t*& out_node)
    {
        if (head == llnode_t::NIL)
            return 0;
        llnode_t* const phead = dexer->idx2obj<llnode_t>(head);
        llindex_t       tail  = phead->m_prev;
        s_remove_item(head, dexer, tail, out_node);
        return 1;
    }

    llnode_t* ll_remove_item(llindex_t& head, dexer_t* dexer, llindex_t item)
    {
        llnode_t* node = nullptr;
        if (head != llnode_t::NIL)
            s_remove_item(head, dexer, item, node);
        return node;
    }

    llnode_t* ll_remove_head(llindex_t& head, dexer_t* dexer)
    {
        llnode_t* node = nullptr;
        if (head != llnode_t::NIL)
            s_remove_item(head, dexer, head, node);
        return node;
    }

    llnode_t* ll_remove_tail(llindex_t& head, dexer_t* dexer)
    {
        llnode_t* node = nullptr;
        if (head != llnode_t::NIL)
            s_remove_tail(head, dexer, node);
        return node;
    }

    llindex_t ll_remove_headi(llindex_t& head, dexer_t* dexer)
    {
        llindex_t item = head;
        if (item != llnode_t::NIL)
        {
            llnode_t* node = nullptr;
            s_remove_item(head, dexer, head, node);
        }
        return item;
    }

    llindex_t ll_remove_taili(llindex_t& head, dexer_t* dexer)
    {
        if (head == llnode_t::NIL)
            return head;
        llnode_t* node;
        s_remove_tail(head, dexer, node);
        return dexer->obj2idx(node);
    }

    void llist_t::insert(dexer_t* dexer, llindex_t item)
    {
        ASSERT(m_size < m_size_max);
        ll_insert(m_head, dexer, item);
        m_size += 1;
    }

    void llist_t::insert_tail(dexer_t* dexer, llindex_t item)
    {
        ASSERT(m_size < m_size_max);
        ll_insert_tail(m_head, dexer, item);
        m_size += 1;
    }

    llnode_t* llist_t::remove_item(dexer_t* dexer, llindex_t item)
    {
        llnode_t* node = nullptr;
        ASSERT(m_size > 0);
        s_remove_item(m_head, dexer, item, node);
        m_size -= 1;
        return node;
    }

    llnode_t* llist_t::remove_head(dexer_t* dexer)
    {
        llnode_t* node = nullptr;
        ASSERT(m_size > 0);
        llindex_t item = m_head;
        m_size -= 1;
        s_remove_item(m_head, dexer, item, node);
        return node;
    }

    llnode_t* llist_t::remove_tail(dexer_t* dexer)
    {
        llnode_t* node = nullptr;
        ASSERT(m_size > 0);
        m_size -= s_remove_tail(m_head, dexer, node);
        return node;
    }

    llindex_t llist_t::remove_headi(dexer_t* dexer)
    {
        llnode_t* node = nullptr;
        ASSERT(m_size > 0);
        llindex_t item = m_head;
        s_remove_item(m_head, dexer, item, node);
        m_size -= 1;
        return item;
    }

    llindex_t llist_t::remove_taili(dexer_t* dexer)
    {
        llnode_t* node = nullptr;
        ASSERT(m_size > 0);
        m_size -= s_remove_tail(m_head, dexer, node);
        return node2idx(dexer, node);
    }

}  // namespace ncore
