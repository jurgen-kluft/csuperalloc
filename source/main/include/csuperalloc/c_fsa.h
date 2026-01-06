#ifndef __C_SUPERALLOC_INTERNAL_FSA_H__
#define __C_SUPERALLOC_INTERNAL_FSA_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

namespace ncore
{
    struct arena_t;

    namespace nfsa
    {
        struct section_t;
        struct block_t;
    }  // namespace nfsa

    struct fsa_t
    {
        arena_t*         m_arena;
        void*            m_address_base;
        u64              m_address_range;
        nfsa::section_t* m_sections;
        s8               m_section_maxsize_shift;
        u32              m_sections_array_size;
        u32              m_sections_free_index;
        u32              m_sections_free_bin0;                // nbinmap10, bin0
        u32*             m_sections_free_bin1;                // nbinmap10, bin1
        u32*             m_active_section_bin0_per_blockcfg;  // segments that still have available free blocks
        u32*             m_active_section_bin1_per_blockcfg;  //
        nfsa::block_t**  m_active_block_list_per_allocsize;   // blocks that still have available free items, a list per allocation size
    };

    namespace nfsa
    {
        void  initialize(fsa_t* fsa, arena_t* arena, u64 address_range, u32 section_size);
        void  deinitialize(fsa_t* fsa);
        void* allocate(fsa_t* fsa, u32 size);
        void  deallocate(fsa_t* fsa, void* ptr);

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
