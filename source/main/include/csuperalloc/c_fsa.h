#ifndef __C_SUPERALLOC_INTERNAL_FSA_H__
#define __C_SUPERALLOC_INTERNAL_FSA_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

namespace ncore
{
    struct fsa_t
    {
        u16 m_sections_free_index;
        u16 m_sections_free_list;
        u16 m_sections_max_index;
        u8  m_section_size_shift;
        u8  m_page_size_shift;
    };

    namespace nfsa
    {
        fsa_t* new_fsa(u64 address_range, u32 section_size);
        void   destroy(fsa_t* fsa);
        void*  allocate(fsa_t* fsa, u32 size);
        void   deallocate(fsa_t* fsa, void* ptr);
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
