#ifndef __C_SUPERALLOC_INTERNAL_POOL_H__
#define __C_SUPERALLOC_INTERNAL_POOL_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

#include "csuperalloc/private/c_list.h"

namespace ncore
{
    struct items_t
    {
        items_t(byte* array, u32 sizeof_item, u32 capacity);
        u32   m_item_capacity;    // maximum number
        u32   m_item_count;       // current number
        u32   m_item_size;        // size of each item
        u32   m_item_free_index;  // index of the first free item
        byte* m_array;            // array of items

        u32   alloc();
        void  dealloc(u32 index);
        u32   idx_of(const void* obj) const { return ((const byte*)obj - m_array) / m_item_size; }
        void* obj_of(u32 index) const { return m_array + (index * m_item_size); }
    };

    template <typename T>
    struct objects_t
    {
        items_t m_items;

        objects_t(T* array, u32 capacity)
            : m_items((byte*)array, sizeof(T), capacity)
        {
        }
        T* alloc()
        {
            const u32 i = m_items.alloc();
            return (i == D_U32_MAX) ? nullptr : (T*)obj_of(i);
        }
        void dealloc(T* obj)
        {
            if (obj != nullptr)
            {
                m_items.dealloc(idx_of(obj));
            }
        }
        u32 idx_of(const T* obj) const { return m_items.idx_of(obj); }
        T*  obj_of(u32 index) const { return (T*)m_items.obj_of(index); }
    };

    template <typename T, typename A>
    objects_t<T> g_allocate_objects(A* allocator, u32 capacity)
    {
        T* array = g_allocate_array<T>(allocator, capacity);
        return objects_t<T>(array, capacity);
    }

    template <typename T>
    struct linked_objects_t
    {
        objects_t<T> m_items;
        llist32_t    m_list;
        u32          m_head;

        linked_objects_t(T* array, u32* next_array, u32 prev_array, u32 capacity)
            : m_items((byte*)array, sizeof(T), capacity)
            , m_list(next_array, prev_array)
            , m_head(nu32::NIL)
        {
        }

        T* alloc()
        {
            if (m_head == nu32::NIL)
                return m_items.alloc();
            u32 index = m_list.pop(m_head);
            return (T*)m_items.obj_of(index);
        }

        void dealloc(T* obj)
        {
            if (obj != nullptr)
            {
                u32 index = idx_of(obj);
                m_list.add(m_head, index);
            }
        }

        u32 idx_of(const T* obj) const { return m_items.idx_of(obj); }
        T*  obj_of(u32 index) const { return (T*)m_items.obj_of(index); }

        void add_to_list(u32& head, u32 index) { m_list.add(head, index); }
        void rem_from_list(u32& head, u32 index) { m_list.rem(head, index); }
    };

    template <typename T, typename A>
    linked_objects_t<T> g_allocate_linked_objects(A* allocator, u32 capacity)
    {
        T*   array      = g_allocate_array<T>(allocator, capacity);
        u32* next_array = g_allocate_array<u32>(allocator, capacity);
        u32* prev_array = g_allocate_array<u32>(allocator, capacity);
        return linked_objects_t<T>(array, next_array, prev_array, capacity);
    }

};  // namespace ncore

#endif
