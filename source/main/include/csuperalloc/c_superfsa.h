#ifndef __C_SUPERALLOC_INTERNAL_FSA_H__
#define __C_SUPERALLOC_INTERNAL_FSA_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

namespace ncore
{
    struct superheap_t;

    struct superfsa_section_t;
    struct superfsa_block_t;

    struct superfsa_t
    {
        superheap_t*        m_heap;
        void*               m_address_base;
        u64                 m_address_range;
        s8                  m_section_maxsize_shift;
        u32                 m_sections_array_size;
        superfsa_section_t* m_sections;
        u32                 m_sections_free_index;
        u32                 m_sections_free_bin0;                // nbinmap10, bin0
        u32*                m_sections_free_bin1;                // nbinmap10, bin1
        u32*                m_active_section_bin0_per_blockcfg;  // segments that still have available free blocks
        u32*                m_active_section_bin1_per_blockcfg;  //
        superfsa_block_t**  m_active_block_list_per_allocsize;   // blocks that still have available free items, a list per allocation size
    };

    void  superfsa_initialize(superfsa_t* fsa, superheap_t* heap, u64 address_range, u32 section_size);
    void  superfsa_deinitialize(superfsa_t* fsa);
    void* superfsa_allocate(superfsa_t* fsa, u32 size, u32 alignment);
    void  superfsa_deallocate(superfsa_t* fsa, void* ptr);

    template<typename T>
    T* g_allocate(superfsa_t* fsa)
    {
        return (T*)superfsa_allocate(fsa, sizeof(T), alignof(T));
    }

    template<typename T>
    T* g_allocate_array(superfsa_t* fsa, u32 count)
    {
        return (T*)superfsa_allocate(fsa, sizeof(T) * count, alignof(T));
    }

    template<typename T>
    void g_deallocate(superfsa_t* fsa, T* ptr)
    {
        superfsa_deallocate(fsa, (void*)ptr);
    }

    template<typename T>
    void g_deallocate_array(superfsa_t* fsa, T* ptr)
    {
        superfsa_deallocate(fsa, (void*)ptr);
    }

};  // namespace ncore

#endif
