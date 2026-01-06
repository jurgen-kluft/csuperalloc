#ifndef __CSUPERALLOC_LIST_UTILS_H_
#define __CSUPERALLOC_LIST_UTILS_H_
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

namespace ncore
{
    // Circular doubly linked list
    template <typename T>
    void ll_insert(T*& head, T* item)
    {
        item->m_next         = item;
        item->m_prev         = item;
        head                 = (head == nullptr) ? item : head;
        item->m_next         = head;
        item->m_prev         = head->m_prev;
        head->m_prev->m_next = item;
        head->m_prev         = item;
    }

    template <typename T>
    T* ll_pop(T*& head)
    {
        T* item = head;
        if (item != nullptr)
        {
            head                 = (head->m_next == head) ? nullptr : head->m_next;
            item->m_prev->m_next = item->m_next;
            item->m_next->m_prev = item->m_prev;
            item->m_prev         = nullptr;
            item->m_next         = nullptr;
        }
        return item;
    }

    template <typename T>
    void ll_remove(T*& head, T* item)
    {
        head                 = (head == item) ? item->m_next : head;
        head                 = (head == item) ? nullptr : head;
        item->m_prev->m_next = item->m_next;
        item->m_next->m_prev = item->m_prev;
        item->m_prev         = nullptr;
        item->m_next         = nullptr;
    }

    struct llist32_t
    {
        llist32_t(u32* array_next, u32* array_prev);
        u32* m_array_next;
        u32* m_array_prev;
        void add(u32& head, u32 index);
        void rem(u32& head, u32 index);
        u32  pop(u32& head);
    };

    struct llist16_t
    {
        llist16_t(u16* array_next, u16* array_prev);
        u16* m_array_next;
        u16* m_array_prev;
        void add(u16& head, u16 index);
        void rem(u16& head, u16 index);
        u16  pop(u16& head);
    };

}  // namespace ncore

#endif  // __CSUPERALLOC_LIST_UTILS_H_
