#ifndef __C_SUPERALLOC_INTERNAL_HEAP_H__
#define __C_SUPERALLOC_INTERNAL_HEAP_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

#include "ccore/c_memory.h"

namespace ncore
{
    struct arena_t;
    struct superheap_t
    {
        arena_t* m_arena;
    };

    void  superheap_initialize(superheap_t* sh, u64 memory_range, u64 size_to_pre_allocate);
    void  superheap_deinitialize(superheap_t* sh);
    void* superheap_allocate(superheap_t* sh, u32 size, u32 alignment);
    void  superheap_deallocate(superheap_t* sh, void* ptr);

    class superheap_alloc_t : public alloc_t
    {
    public:
        superheap_t* m_superheap;
        superheap_alloc_t(superheap_t* sh)
            : m_superheap(sh)
        {
        }

    protected:
        virtual void* v_allocate(u32 size, u32 align) override { return superheap_allocate(m_superheap, size, align); }
        virtual void  v_deallocate(void* p) override { superheap_deallocate(m_superheap, p); }
    };

    template <typename T>
    inline T* g_allocate(superheap_t* heap)
    {
        return (T*)superheap_allocate(heap, sizeof(T), alignof(T));
    }

    template <typename T>
    inline T* g_deallocate(superheap_t* heap, T* ptr)
    {
        superheap_deallocate(heap, ptr);
        return nullptr;
    }

    template <typename T>
    inline T* g_allocate_array(superheap_t* heap, u32 count)
    {
        return (T*)superheap_allocate(heap, sizeof(T) * count, alignof(T));
    }

    template <typename T>
    inline T* g_allocate_array_and_clear(superheap_t* heap, u32 count)
    {
        T* ptr = (T*)superheap_allocate(heap, sizeof(T) * count, alignof(T));
        nmem::memset(ptr, 0, sizeof(T) * count);
        return ptr;
    }

    template <typename T>
    inline T* g_allocate_array_and_memset(superheap_t* heap, u32 count, u32 fill)
    {
        T* ptr = (T*)superheap_allocate(heap, sizeof(T) * count, alignof(T));
        nmem::memset(ptr, fill, sizeof(T) * count);
        return ptr;
    }

};  // namespace ncore

#endif
