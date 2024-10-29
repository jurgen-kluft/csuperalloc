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

}  // namespace ncore

#endif  // __CSUPERALLOC_DOUBLY_LINKED_LIST_H_
