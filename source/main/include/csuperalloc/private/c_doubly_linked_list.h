#ifndef __CSUPERALLOC_DOUBLY_LINKED_LIST_H_
#define __CSUPERALLOC_DOUBLY_LINKED_LIST_H_
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

#include "cbase/c_allocator.h"
#include "ccore/c_debug.h"

namespace ncore
{
    typedef u32 llindex_t;

    struct llnode_t
    {
        static const u32 NIL = 0xFFFFFFFF;
        inline bool      is_linked() const { return m_prev != NIL && m_next != NIL; }
        llindex_t        m_prev, m_next;
    };

    inline void ll_reset(llindex_t& head) { head = llnode_t::NIL; }
    inline bool ll_is_nil(llindex_t head) { return head == llnode_t::NIL; }
    void        ll_insert(llindex_t& head, dexer_t* dexer, llindex_t item);       // Inserts 'item' at the head
    void        ll_insert_tail(llindex_t& head, dexer_t* dexer, llindex_t item);  // Inserts 'item' at the tail end
    llnode_t*   ll_remove_item(llindex_t& head, dexer_t* dexer, llindex_t item);
    llnode_t*   ll_remove_head(llindex_t& head, dexer_t* dexer);
    llnode_t*   ll_remove_tail(llindex_t& head, dexer_t* dexer);
    llindex_t   ll_remove_headi(llindex_t& head, dexer_t* dexer);
    llindex_t   ll_remove_taili(llindex_t& head, dexer_t* dexer);

    inline llnode_t* ll_idx2node(dexer_t* dexer, llindex_t i) { return dexer->idx2obj<llnode_t>(i); }
    inline llindex_t ll_node2idx(dexer_t* dexer, llnode_t* node) { return dexer->obj2idx(node); }

    struct llist_t
    {
        inline llist_t()
            : m_size(0)
            , m_size_max(0)
        {
        }
        inline llist_t(u32 size, u32 size_max)
            : m_size(size)
            , m_size_max(size_max)
        {
        }

        inline u32  size() const { return m_size; }
        inline bool is_empty() const { return m_size == 0; }
        inline bool is_full() const { return m_size == m_size_max; }

        inline void init() { reset(); }
        inline void reset()
        {
            m_size = 0;
            m_head = llnode_t::NIL;
        }

        void      insert(dexer_t* dexer, llindex_t item);       // Inserts 'item' at the head
        void      insert_tail(dexer_t* dexer, llindex_t item);  // Inserts 'item' at the tail end
        llnode_t* remove_item(dexer_t* dexer, llindex_t item);
        llnode_t* remove_head(dexer_t* dexer);
        llnode_t* remove_tail(dexer_t* dexer);
        llindex_t remove_headi(dexer_t* dexer);
        llindex_t remove_taili(dexer_t* dexer);

        llnode_t* idx2node(dexer_t* dexer, llindex_t i) const
        {
            ASSERT(i < m_size_max);
            return ll_idx2node(dexer, i);
        }

        llindex_t node2idx(dexer_t* dexer, llnode_t* node) const
        {
            llindex_t i = ll_node2idx(dexer, node);
            ASSERT(i < m_size_max);
            return i;
        }

        u32       m_size;
        u32       m_size_max;
        llindex_t m_head;
    };

}  // namespace ncore

#endif  // __CSUPERALLOC_DOUBLY_LINKED_LIST_H_
