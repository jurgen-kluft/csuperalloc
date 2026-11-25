#ifndef __C_SUPERALLOC_V3_PRIVATE_H__
#define __C_SUPERALLOC_V3_PRIVATE_H__

#include "ccore/c_target.h"

namespace ncore
{
    namespace nsuperallocv3
    {
        struct region_config_t
        {
            u8 m_region_size_shift;
            u8 m_chunk_size_shift;
        };

        struct chunk_config_t
        {
            u8 m_chunk_size_shift;
            u8 m_item_cap_shift;
        };

        struct alloc_config_t
        {
            u8 m_alloc_size_index;   // Zero-based allocation size class
            u8 m_region_size_shift;  // Shift for the region size
            u8 m_chunk_size_shift;   // Shift for the chunk size
            u8 m_item_cap_shift;     // Shift for the item capacity
        };

        struct bin_config_t
        {
            u8 m_alloc_size_index;  // Zero-based allocation size class
            u8 m_chunk_cfg_index;   // Index into the chunk configuration array
            u8 m_region_cfg_index;  // Index into the region configuration array
            u8 m_reserved;          // Reserved for future use
        };

        struct arena_pool_t
        {
            u8    m_reserved;
            u8    m_page_sizeshift;
            u16   m_allocation_size;
            u32   m_free_list;
            u32   m_committed_pages;
            u32   m_reserved_pages;
            byte* m_memory_base;
            byte* m_memory_cursor;
        };

        struct allocator_t
        {
            u32* m_not_full_o_chunks_per_alloc_index;  // Indexed by zero-based allocation size class.
            u32* m_cached_empty_o_chunks_per_chunk_index;
            u32* m_active_o_region_per_region_index;
            u32* m_unused_o_region_per_region_index;

            u16              m_num_bin_configs;
            u16              m_num_chunk_configs;
            u16              m_num_region_configs;
            u8               m_size_class_divisor;  // D, either 2 or 4
            bin_config_t*    m_bin_config_array;    // Ordered by zero-based allocation size class.
            chunk_config_t*  m_chunk_config_array;
            region_config_t* m_region_config_array;

            s8   m_min_region_sizeshift;
            s8   m_arena_size_sizeshift;
            u32* m_o_region_lookup_array;

            byte* m_memory_base;
            u64   m_memory_size;

            byte*        m_arena_memory_base;
            u64          m_arena_memory_size;
            arena_pool_t m_arenas[12];

            byte* m_config_memory_base;
            u64   m_config_memory_size;
        };

        // The allocator must be zero-initialized. managed_size is a nonzero multiple
        // of 16 MiB; pool_size is a power of two, at least 8192 and one system page.
        // configs are used only during initialization and must contain consecutive
        // indices starting at zero. D must be 2 or 4; minimum_size and alignment
        // must both be 8. The largest configured class must not exceed 2 GiB.
        // On failure, call shutdown to retry releasing any remaining reservation.
        allocator_t* create_v3(u64 managed_size, u32 pool_size, const alloc_config_t* configs, u32 count, u32 minimum_size, u32 alignment, u32 D);
        bool         shutdown(allocator_t* allocator);

        bool                s_build_chunk_configs(const alloc_config_t* configs, u32 count, chunk_config_t* chunks, u8* presence, u16& chunk_count);
        bool                s_build_region_configs(const alloc_config_t* configs, u32 count, region_config_t* regions, u8* presence, u16& region_count);
        const bin_config_t* s_get_bin_config_for_size(const allocator_t* allocator, u32 alloc_size);

        void* s_allocate(arena_pool_t* arena);
        void  s_free(arena_pool_t* arena, void* ptr);
    }  // namespace nsuperallocv3
}  // namespace ncore

#endif
