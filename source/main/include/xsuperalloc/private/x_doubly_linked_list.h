#ifndef _X_XVMEM_DOUBLY_LINKED_LIST_H_
#define _X_XVMEM_DOUBLY_LINKED_LIST_H_
#include "xbase/x_target.h"
#ifdef USE_PRAGMA_ONCE
#pragma once
#endif

#include "xbase/x_debug.h"

namespace xcore
{
    typedef u32      llindex_t;

    struct llnode_t
    {
        static const u32 NIL = 0xFFFFFFFF;
        inline bool is_linked() const { return m_prev != NIL && m_next != NIL; }
        llindex_t m_prev, m_next;
    };

    struct lldata_t;

    struct llhead_t
    {
        llindex_t m_index;

        inline llhead_t()
            : m_index(llnode_t::NIL)
        {
        }

        void      reset() { m_index = llnode_t::NIL; }
        bool      is_nil() const { return m_index == llnode_t::NIL; }
        void      insert(lldata_t& data, llindex_t item);      // Inserts 'item' at the head
        void      insert_tail(lldata_t& data, llindex_t item); // Inserts 'item' at the tail end
        llnode_t* remove_item(lldata_t& data, llindex_t item);
        llnode_t* remove_head(lldata_t& data);
        llnode_t* remove_tail(lldata_t& data);
        llindex_t remove_headi(lldata_t& data);
        llindex_t remove_taili(lldata_t& data);

        inline void operator=(u16 i) { m_index = i; }
        inline void operator=(const llindex_t& index) { m_index = index; }
        inline void operator=(const llhead_t& head) { m_index = head.m_index; }
    };

    struct lldata_t
    {
        void* m_data;
        u32 m_pagesize;
        u32 m_itemsize;

        llnode_t* idx2node(llindex_t i)
        {
            if (i == llnode_t::NIL)
                return nullptr;
            return (llnode_t*)((uptr)m_data + ((uptr)m_pagesize * (i >> 16)) + ((uptr)m_itemsize * (i & 0xFFFF)));
        }

        llindex_t node2idx(llnode_t* node)
        {
            if (node == nullptr)
                return llindex_t();
            u32 const page_index = (u32)(((uptr)node - (uptr)m_data) / m_pagesize);
            u32 const item_index = (u32)(((uptr)node - (uptr)m_data) & (m_pagesize - 1)) / m_itemsize;
            return (page_index << 16) | item_index;
        }
    };

    struct llist_t
    {
        inline llist_t()
            : m_size(0)
            , m_size_max(0)
        {
        }
        inline llist_t(u16 size, u16 size_max)
            : m_size(size)
            , m_size_max(size_max)
        {
        }

        inline u32  size() const { return m_size; }
        inline bool is_empty() const { return m_size == 0; }
        inline bool is_full() const { return m_size == m_size_max; }

        void        initialize(lldata_t& data, u16 start, u16 size, u16 max_size);
        inline void reset()
        {
            m_size = 0;
            m_head.reset();
        }

        void      insert(lldata_t& data, llindex_t item);      // Inserts 'item' at the head
        void      insert_tail(lldata_t& data, llindex_t item); // Inserts 'item' at the tail end
        llnode_t* remove_item(lldata_t& data, llindex_t item);
        llnode_t* remove_head(lldata_t& data);
        llnode_t* remove_tail(lldata_t& data);
        llindex_t remove_headi(lldata_t& data);
        llindex_t remove_taili(lldata_t& data);

        llnode_t* idx2node(lldata_t& data, llindex_t i) const
        {
            ASSERT(i < m_size_max);
            return data.idx2node(i);
        }

        llindex_t node2idx(lldata_t& data, llnode_t* node) const
        {
            llindex_t i = data.node2idx(node);
            ASSERT(i < m_size_max);
            return i;
        }

        u16      m_size;
        u16      m_size_max;
        llhead_t m_head;
    };

} // namespace xcore

#endif // _X_XVMEM_DOUBLY_LINKED_LIST_H_