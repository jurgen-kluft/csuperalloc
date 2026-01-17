#ifndef __C_SUPERALLOC_INTERNAL_FSA_H__
#define __C_SUPERALLOC_INTERNAL_FSA_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

namespace ncore
{
    struct fsa_t;

    namespace nfsa
    {
        // number of 64 KiB blocks, maximum is 65534 blocks
        fsa_t* new_fsa(u16 num_blocks=1024);
        void   destroy(fsa_t* fsa);
        // minimum alloc size is 8 bytes, maximum alloc size is 32 KiB
        void*  allocate(fsa_t* fsa, u32 size);
        void   deallocate(fsa_t* fsa, void* ptr);
        u32    get_size(fsa_t* fsa, void* ptr);
        u32    ptr2idx(fsa_t* fsa, void* ptr);
        void*  idx2ptr(fsa_t* fsa, u32 index);
    }  // namespace nfsa

    template <typename T>
    T* g_allocate(fsa_t* fsa)
    {
        return (T*)nfsa::allocate(fsa, sizeof(T));
    }

    template <typename T>
    T* g_allocate_array(fsa_t* fsa, u32 count)
    {
        return (T*)nfsa::allocate(fsa, sizeof(T) * count);
    }

    template <typename T>
    void g_deallocate(fsa_t* fsa, T* ptr)
    {
        nfsa::deallocate(fsa, (void*)ptr);
    }

    template <typename T>
    void g_deallocate_array(fsa_t* fsa, T* ptr)
    {
        nfsa::deallocate(fsa, (void*)ptr);
    }

};  // namespace ncore

#endif
