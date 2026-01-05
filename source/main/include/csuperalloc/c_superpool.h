#ifndef __C_SUPERALLOC_INTERNAL_POOL_H__
#define __C_SUPERALLOC_INTERNAL_POOL_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

namespace ncore
{

    template <typename T>
    struct pool_t
    {
        u32 m_capacity;   // maximum number
        u32 m_count;      // current number
        T*  m_free_list;  // free list of chunks
        T*  m_array;      // array of chunks (heap)

        u32 idx_of(T* obj) const { return (u32)(obj - m_array); }
        T* obj_of(u32 index) const { return &m_array[index]; }
        T* alloc();
        void dealloc(u32 index);
        void add_to_list(u32& head);
        void remove_from_list(u32& head, u32 index);
    };

};  // namespace ncore

#endif
