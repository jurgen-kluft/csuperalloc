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

        struct item_info_t
        {
            u32 m_item_size;
            u32 m_item_capacity;
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

        struct region_t
        {
            u8  m_type;  // 2, region
            u8  m_region_cfg_index;
            u16 m_reserved0;
            u16 m_count;
            u16 m_capacity;
            u16 m_free_slot_index;  // index of the first never-used slot
            u16 m_free_list;        // 0xffff when empty
            u32 m_index;            // index in the 16 MiB region lookup table
            u32 m_o_array;          // arena offset of the slot array
            u32 m_o_next;           // arena offset of the next active region
        };

        struct chunk_t
        {
            u8  m_type;
            u8  m_bin_index;
            u16 m_index;
            u32 m_o_chunk2;
            u32 m_o_tags;
            u32 m_o_region;
            u32 m_o_next;
        };

        struct chunk2_t
        {
            u64 m_bin0;
            u32 m_o_bin1;
            u16 m_item_free_index;
            u16 m_item_capacity;
            u16 m_item_count;
            u16 m_item_size;
        };

        struct block_t
        {
            u8  m_type;
            u8  m_bin_index;
            u16 m_index;
            u32 m_pages;
            u32 m_o_region;
            u32 m_tag;
            u32 m_o_next;
        };

        struct allocator_t
        {
            u32*         m_not_full_o_chunks_per_alloc_index;  // Indexed by zero-based allocation size class.
            u32*         m_cached_empty_o_chunks_per_chunk_index;
            u32*         m_active_o_region_per_region_index;
            u32*         m_unused_o_region_per_region_index;
            item_info_t* m_item_info_per_alloc_index;

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
            u32   m_next_region_index;

            byte*        m_arena_memory_base;
            u64          m_arena_memory_size;
            arena_pool_t m_arenas[12];

            byte* m_config_memory_base;
            u64   m_config_memory_size;
        };

        // managed_size is a nonzero multiple of 16 MiB; pool_size is a power of two,
        // at least 8192 and one system page.
        // configs are used only during initialization and must contain consecutive
        // indices starting at zero. D must be 2 or 4; minimum_size and alignment
        // must both be 8. The largest configured class must not exceed 2 GiB.
        // Each region is at least 16 MiB, with page-sized or larger slots and
        // at most 2048 slots. Its size and slot shifts must be below 32;
        // chunk item capacity shifts must be at most 12, and each aligned item size
        // must fit in its configured chunk/block size.
        // A successful shutdown invalidates the allocator pointer. If shutdown fails,
        // the allocator remains valid only for retrying shutdown.
        allocator_t* create_allocator(u64 managed_size, u32 pool_size, const alloc_config_t* configs, u32 count, u32 minimum_size, u32 alignment, u32 D);
        bool         shutdown(allocator_t* allocator);

        bool                s_build_chunk_configs(const alloc_config_t* configs, u32 count, chunk_config_t* chunks, u8* presence, u8& chunk_count);
        bool                s_build_region_configs(const alloc_config_t* configs, u32 count, region_config_t* regions, u8* presence, u8& region_count);
        const bin_config_t* s_get_bin_config_for_size(const allocator_t* allocator, u32 alloc_size);
        // allocator and region_config_index must identify a valid configuration.
        region_t* s_create_region(allocator_t* allocator, u8 region_config_index);
        // Push requires a region with capacity not already on a list; pop returns nullptr if empty.
        void      s_push_active_region(allocator_t* allocator, region_t* region);
        region_t* s_pop_active_region(allocator_t* allocator, u8 region_config_index);
        // Slot helpers do not change the region's count or active-list membership.
        // Pop returns -1 when no virgin or recycled slot remains.
        s32  s_pop_region_slot(allocator_t* allocator, region_t* region);
        void s_push_region_slot(allocator_t* allocator, region_t* region, u16 index);
        // Push requires a not-full chunk not already on a list; pop returns nullptr if empty.
        void     s_push_not_full_chunk(allocator_t* allocator, chunk_t* chunk);
        chunk_t* s_pop_not_full_chunk(allocator_t* allocator, u8 bin_index);
        // region must be the active-list head for the bin's region configuration
        // and have an available slot. Chunk item size must not exceed 65535;
        // larger items must use blocks.
        // Chunk creation commits the whole chunk before publishing it.
        chunk_t* s_create_chunk(allocator_t* allocator, region_t* region, u8 bin_index);
        block_t* s_create_block(allocator_t* allocator, region_t* region, u8 bin_index);
        // block must be live. On decommit failure, its metadata remains allocated.
        bool s_free_block(allocator_t* allocator, block_t* block);
        // chunk must be the not-full-list head for its allocation bin.
        void* s_allocate_chunk_item(allocator_t* allocator, chunk_t* chunk);
        // item must be a live allocation belonging to chunk.
        void s_free_chunk_item(allocator_t* allocator, chunk_t* chunk, void* item);

        void* s_allocate(arena_pool_t* arena);
        void  s_free(arena_pool_t* arena, void* ptr);
    }  // namespace nsuperallocv3
}  // namespace ncore

#endif
