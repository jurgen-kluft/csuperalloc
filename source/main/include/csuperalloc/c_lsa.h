#ifndef __C_SUPERALLOC_INTERNAL_LSA_H__
#define __C_SUPERALLOC_INTERNAL_LSA_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

namespace ncore
{
    struct lsa_t;

    namespace nlsa
    {
        // lsa = large size allocator
        // Note: it can handle an address range of 1 GiB
        // example: to be able to handle allocation sizes from 32 KiB up to 512 MiB, you
        // should create multiple lsa_t instances:
        //    lsa_t* lsa_64KiB   = nlsa::new_lsa(64 * 1024, 16384);     // number of 64 KiB blocks
        //    lsa_t* lsa_128KiB  = nlsa::new_lsa(128 * 1024, 8192);     // number of 128 KiB blocks
        //    lsa_t* lsa_256KiB  = nlsa::new_lsa(256 * 1024, 4096);     // number of 256 KiB blocks
        //    lsa_t* lsa_512KiB  = nlsa::new_lsa(512 * 1024, 2048);     // number of 512 KiB blocks
        //    lsa_t* lsa_1MiB    = nlsa::new_lsa(1024 * 1024, 1024);    // number of 1 MiB blocks
        //    lsa_t* lsa_2MiB    = nlsa::new_lsa(2 * 1024 * 1024, 512); // number of 2 MiB blocks
        //    lsa_t* lsa_4MiB    = nlsa::new_lsa(4 * 1024 * 1024, 256); // number of 4 MiB blocks
        //    lsa_t* lsa_8MiB    = nlsa::new_lsa(8 * 1024 * 1024,128);  // number of 8 MiB blocks
        //    lsa_t* lsa_16MiB   = nlsa::new_lsa(16 * 1024 * 1024, 64); // number of 16 MiB blocks
        //    lsa_t* lsa_32MiB   = nlsa::new_lsa(32 * 1024 * 1024, 32); // number of 32 MiB blocks
        //    lsa_t* lsa_64MiB   = nlsa::new_lsa(64 * 1024 * 1024, 16); // number of 64 MiB blocks
        //    lsa_t* lsa_128MiB  = nlsa::new_lsa(128 * 1024 * 1024, 8); // number of 128 MiB blocks
        //    lsa_t* lsa_256MiB  = nlsa::new_lsa(256 * 1024 * 1024, 4); // number of 256 MiB blocks
        //    lsa_t* lsa_512MiB  = nlsa::new_lsa(512 * 1024 * 1024, 2); // number of 512 MiB blocks

        // Note: Perhaps we need a method to new_lsa that takes an already reserved address space
        //       and a separate address space for the book-keeping data (lsa_t + block_t array).

        // number of 64 KiB blocks, maximum of 32768 blocks
        lsa_t* new_lsa(void* data, u32& data_page_offset, void* base, u32& base_page_offset, u32 sizeof_block = 64 * 1024, u16 num_blocks = 1024);
        lsa_t* new_lsa(u32 sizeof_block = 64 * 1024, u16 num_blocks = 1024);
        void   destroy(lsa_t* lsa);
        void*  allocate(lsa_t* lsa, u32 size);
        void   deallocate(lsa_t* lsa, void* ptr);
        u32    get_size(lsa_t* lsa, void* ptr);
    }  // namespace nlsa

    template <typename T>
    T* g_allocate(lsa_t* lsa)
    {
        return (T*)nlsa::allocate(lsa, sizeof(T));
    }

    template <typename T>
    T* g_allocate_array(lsa_t* lsa, u32 count)
    {
        return (T*)nlsa::allocate(lsa, sizeof(T) * count);
    }

    template <typename T>
    T* g_allocate_array_and_clear(lsa_t* lsa, u32 count)
    {
        T* array = (T*)nlsa::allocate(lsa, sizeof(T) * count);
    }

    template <typename T>
    void g_deallocate(lsa_t* lsa, T* ptr)
    {
        nlsa::deallocate(lsa, (void*)ptr);
    }

    template <typename T>
    void g_deallocate_array(lsa_t* lsa, T* ptr)
    {
        nlsa::deallocate(lsa, (void*)ptr);
    }

};  // namespace ncore

#endif  // __C_SUPERALLOC_INTERNAL_LSA_H__
