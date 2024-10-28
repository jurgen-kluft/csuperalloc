#ifndef __C_SUPERALLOC_ALLOCATOR_CONFIG_H__
#define __C_SUPERALLOC_ALLOCATOR_CONFIG_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

namespace ncore
{
    struct chunkconfig_t
    {
        s16 m_sizeshift;  // The shift of the chunk size (e.g. 12 for 4KB)
        s16 m_index;      // The index of this chunk config in the chunk config array
    };

    struct sectionconfig_t
    {
        const s16 m_sizeshift;  // The shift of the section size (e.g. 26 for 64MB)
        const s16 m_index;      // The index of this config in the section config array
    };

    struct binconfig_t
    {
        binconfig_t(u16 alloc_bin_index, u32 alloc_size, chunkconfig_t const& chunk_config)
            : m_alloc_bin_index(alloc_bin_index)
            , m_alloc_size(alloc_size)
            , m_chunk_config(chunk_config)
            , m_max_alloc_count((((u32)1 << chunk_config.m_sizeshift) / alloc_size))
        {
        }
        binconfig_t(const binconfig_t& other)
            : m_alloc_bin_index(other.m_alloc_bin_index)
            , m_alloc_size(other.m_alloc_size)
            , m_chunk_config(other.m_chunk_config)
            , m_max_alloc_count(other.m_max_alloc_count)
        {
        }
        const u64           m_alloc_bin_index : 16;  // The index of this bin
        const u64           m_alloc_size : 48;       // The size of the allocation that this bin is managing
        const chunkconfig_t m_chunk_config;          // The index of the chunk size that this bin requires
        const u32           m_max_alloc_count;       // The maximum number of allocations that can be made from a single chunk
    };

    struct superalloc_config_t
    {
        superalloc_config_t()
            : m_total_address_size(0)
            , m_segment_address_range(0)
            , m_segment_address_range_shift(0)
            , m_internal_heap_address_range(0)
            , m_internal_heap_pre_size(0)
            , m_internal_fsa_address_range(0)
            , m_internal_fsa_segment_size(0)
            , m_internal_fsa_pre_size(0)
            , m_num_binconfigs(0)
            , m_num_chunkconfigs(0)
            , m_abinconfigs(nullptr)
        {
        }

        virtual binconfig_t const& size2bin(u32 size) const = 0;

        u64                m_total_address_size;
        u64                m_segment_address_range;
        s8                 m_segment_address_range_shift;
        u32                m_internal_heap_address_range;
        u32                m_internal_heap_pre_size;
        u32                m_internal_fsa_address_range;
        u32                m_internal_fsa_segment_size;
        u32                m_internal_fsa_pre_size;
        u32                m_num_binconfigs;
        u32                m_num_chunkconfigs;
        binconfig_t const* m_abinconfigs;
    };

    extern superalloc_config_t const* gGetSuperAllocConfigWindowsDesktopApp10p();
    extern superalloc_config_t const* gGetSuperAllocConfigWindowsDesktopApp25p();

};  // namespace ncore

#endif
