#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_arena.h"
#include "ccore/c_bitvec.h"
#include "ccore/c_statevec.h"
#include "ccore/c_debug.h"
#include "ccore/c_limits.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"

namespace ncore
{
    namespace nsuperallocv3
    {

#define CSUPERALLOC_V3_D2

        enum
        {
            c4KiB   = 12,
            c8KiB   = 13,
            c16KiB  = 14,
            c32KiB  = 15,
            c64KiB  = 16,
            c128KiB = 17,
            c256KiB = 18,
            c512KiB = 19,
            c1MiB   = 20,
            c2MiB   = 21,
            c4MiB   = 22,
            c8MiB   = 23,
            c16MiB  = 24,
            c32MiB  = 25,
            c64MiB  = 26,
            c128MiB = 27,
            c256MiB = 28,
            c512MiB = 29,
            c1GiB   = 30,
            c2GiB   = 31,
            c4GiB   = 32,
        };

        enum offset_id_t
        {
            c_offset_nil        = 0x0fffffff,
            c_offset_value_mask = 0x0fffffff,
            c_offset_id_mask    = 0xf,
            c_offset_id_shift   = 28,
        };

        typedef u32 arena_offset_t;

        static inline u32 s_offset_value(arena_offset_t offset) { return static_cast<u32>(offset) & c_offset_value_mask; }
        static inline u8  s_offset_id(arena_offset_t offset) { return static_cast<u8>((offset >> c_offset_id_shift) & c_offset_id_mask); }

        template <typename T>
        static inline T* s_offset_to_ptr(arena_offset_t offset, void* arena_base)
        { return static_cast<T*>(static_cast<u8*>(arena_base) + s_offset_value(offset)); }

        // A chunk based region consists of N chunks
        // A chunk consists of N elements with a maximum of 2048.
        // Note: Bitvector should be lazily initialized.
        // Size: 24 bytes (must be <= 24 bytes)
        struct chunk_t
        {
            u16            m_capacity;         // max number of elements
            u16            m_count;            // cur number of elements
            u8             m_alloc_cfg_index;  // allocation configuration index for this chunk
            u8             m_reserved1;        // reserved for future use
            u16            m_reserved2;        // reserved for future use
            arena_offset_t m_o_region;         // region this chunk belongs to (index into allocator->m_region_array)
            u16            m_index;            // index into region->m_o_array
            u16            m_free_item_index;  // index to first virgin item in the chunk
            u32            m_bin0;             // bitvec level 0
            arena_offset_t m_o_bin1;           // bitvec level 1 (offset into arena memory to u32[])
        };

        // A block based region consists of N blocks
        // A block is a fixed size number of pages (power-of-two number of pages).
        // But an allocation can use less pages than the block size, so we need to track
        // the number of committed pages per block.
        // Size: 20 bytes (must be <= 24 bytes)
        struct block_t
        {
            u32            m_pages;            // number of committed pages in the block
            u8             m_alloc_cfg_index;  // allocation configuration index for this block
            u8             m_reserved1;        // reserved for future use
            u16            m_reserved2;        // reserved for future use
            arena_offset_t m_o_region;         // region this block belongs to (offset into arena memory to a region_t)
            u32            m_index;            // index into region->m_o_array
            u32            m_tag;              // tag for the block (e.g., for tracking usage or type)
            arena_offset_t m_o_next;           // next block in list (offset into arena memory)
        };

        struct region_config_t
        {
            u8 m_type;               // 0 for chunk-based, 1 for block-based
            u8 m_config_index;       // index of the region configuration
            u8 m_region_size_shift;  // size of the region in power-of-two pages
            u8 m_chunk_size_shift;   // size of the chunk/block in power-of-two pages
        };

        struct chunk_config_t
        {
            u8 m_type;            // 0 for chunk-based, 1 for block-based
            u8 m_config_index;    // index of the chunk configuration
            u8 m_size_shift;      // size of the chunk in power-of-two pages
            u8 m_item_cap_shift;  // capacity of items in the chunk in power-of-two
        };

        // A region consists of N chunks/blocks
        // Example size of a region: 16 MB
        // Note: A region is locked to a specific region configuration
        // Size: 20 bytes (must be <= 24 bytes)
        struct region_t
        {
            u8             m_alloc_cfg_index;  // allocation configuration index for this region
            u8             m_reserved1;        // reserved bits for alignment or future use
            u16            m_reserved2;        // reserved for future use
            u32            m_index;            // index into allocator->m_o_region_lookup_array
            u16            m_free_slot_index;  // index of the first virgin chunk/block slot in m_array
            u16            m_free_list;        // index of first slot of the free list for chunks/blocks
            arena_offset_t m_o_array;          // array of chunk/block offsets (offset into arena memory to a array_t)
            arena_offset_t m_o_tags;           // array for handling tags (offset into arena memory to a array_t)
        };

        // Size: 16 bytes
        struct alloc_config_t
        {
            u8              m_reindex;        // reindex value for the allocation configuration
            u8              m_reserved1;      // reserved for future use
            u16             m_reserved2;      // reserved for future use
            u32             m_alloc_size;     // allocation size
            region_config_t m_region_config;  // region configuration
            chunk_config_t  m_chunk_config;   // chunk configuration
        };

        // clang-format off
#define D_ALLOC_CONFIG(reindex, alloc_size, region_cfg_index, region_size_shift, chunk_cfg_index, chunk_size_shift, chunk_item_cap_shift) \
    {                                                                                                                                     \
      (reindex),                                                                                                                          \
      0,0,                                                                                                                                \
      (alloc_size),                                                                                                                       \
      {(chunk_item_cap_shift == 0) ? 1 : 0, region_cfg_index, region_size_shift, chunk_size_shift},                                       \
      {(chunk_item_cap_shift == 0) ? 1 : 0, chunk_cfg_index, chunk_size_shift, chunk_item_cap_shift},                                     \
    }
        // clang-format on

#ifdef CSUPERALLOC_V3_D2
        // D == 2
        static alloc_config_t s_alloc_configs[] = {
          D_ALLOC_CONFIG(0, 8, 0, 0, c16MiB, c16KiB, 2048),                 //
          D_ALLOC_CONFIG(2, 12, 0, 0, c16MiB, c16KiB, 1024),                //
          D_ALLOC_CONFIG(2, 16, 0, 0, c16MiB, c16KiB, 1024),                //
          D_ALLOC_CONFIG(3, 24, 1, 0, c16MiB, c16KiB, 1024),                //
          D_ALLOC_CONFIG(4, 32, 1, 0, c16MiB, c32KiB, 1024),                //
          D_ALLOC_CONFIG(5, 48, 1, 0, c16MiB, c32KiB, 1024),                //
          D_ALLOC_CONFIG(6, 64, 2, 0, c16MiB, c64KiB, 1024),                //
          D_ALLOC_CONFIG(7, 96, 2, 0, c16MiB, c64KiB, 1024),                //
          D_ALLOC_CONFIG(8, 128, 2, 0, c16MiB, c64KiB, 512),                //
          D_ALLOC_CONFIG(9, 192, 2, 0, c16MiB, c64KiB, 512),                //
          D_ALLOC_CONFIG(10, 256, 2, 0, c16MiB, c64KiB, 256),               //
          D_ALLOC_CONFIG(11, 384, 2, 0, c16MiB, c64KiB, 256),               //
          D_ALLOC_CONFIG(12, 512, 2, 0, c16MiB, c64KiB, 128),               //
          D_ALLOC_CONFIG(13, 768, 2, 0, c16MiB, c64KiB, 128),               //
          D_ALLOC_CONFIG(14, 1 * cKB, 2, 0, c16MiB, c64KiB, 64),            //
          D_ALLOC_CONFIG(15, 1 * cKB + 512, 2, 0, c16MiB, c64KiB, 64),      //
          D_ALLOC_CONFIG(16, 2 * cKB, 2, 0, c16MiB, c64KiB, 32),            //
          D_ALLOC_CONFIG(17, 3 * cKB, 2, 0, c16MiB, c64KiB, 32),            //
          D_ALLOC_CONFIG(18, 4 * cKB, 2, 0, c16MiB, c64KiB, 16),            //
          D_ALLOC_CONFIG(19, 6 * cKB, 2, 0, c16MiB, c64KiB, 16),            //
          D_ALLOC_CONFIG(20, 8 * cKB, 2, 0, c16MiB, c64KiB, 16),            //
          D_ALLOC_CONFIG(21, 12 * cKB, 3, 0, c16MiB, c64KiB, 0),            //
          D_ALLOC_CONFIG(22, 16 * cKB, 3, 0, c16MiB, c64KiB, 0),            //
          D_ALLOC_CONFIG(23, 24 * cKB, 3, 0, c16MiB, c64KiB, 0),            //
          D_ALLOC_CONFIG(24, 32 * cKB, 3, 0, c16MiB, c64KiB, 0),            //
          D_ALLOC_CONFIG(25, 48 * cKB, 3, 0, c16MiB, c64KiB, 0),            //
          D_ALLOC_CONFIG(26, 64 * cKB, 3, 0, c16MiB, c64KiB, 0),            //
          D_ALLOC_CONFIG(27, 96 * cKB, 4, 0, c16MiB, c128KiB, 0),           //
          D_ALLOC_CONFIG(28, 128 * cKB, 4, 0, c16MiB, c128KiB, 0),          //
          D_ALLOC_CONFIG(29, 192 * cKB, 5, 0, c16MiB, c256KiB, 0),          //
          D_ALLOC_CONFIG(30, 256 * cKB, 5, 0, c16MiB, c256KiB, 0),          //
          D_ALLOC_CONFIG(31, 384 * cKB, 6, 0, c16MiB, c512KiB, 0),          //
          D_ALLOC_CONFIG(32, 512 * cKB, 6, 0, c16MiB, c512KiB, 0),          //
          D_ALLOC_CONFIG(33, 768 * cKB, 7, 0, c32MiB, c1MiB, 0),            //
          D_ALLOC_CONFIG(34, 1 * cMB, 7, 0, c32MiB, c1MiB, 0),              //
          D_ALLOC_CONFIG(35, 1 * cMB + 512 * cKB, 8, 0, c32MiB, c2MiB, 0),  //
          D_ALLOC_CONFIG(36, 2 * cMB, 8, 0, c32MiB, c2MiB, 0),              //
          D_ALLOC_CONFIG(37, 3 * cMB, 9, 0, c32MiB, c4MiB, 0),              //
          D_ALLOC_CONFIG(38, 4 * cMB, 9, 0, c32MiB, c4MiB, 0),              //
          D_ALLOC_CONFIG(39, 6 * cMB, 10, 0, c32MiB, c8MiB, 0),             //
          D_ALLOC_CONFIG(40, 8 * cMB, 10, 0, c32MiB, c8MiB, 0),             //
          D_ALLOC_CONFIG(41, 12 * cMB, 11, 0, c256MiB, c16MiB, 0),          //
          D_ALLOC_CONFIG(42, 16 * cMB, 11, 0, c256MiB, c16MiB, 0),          //
          D_ALLOC_CONFIG(43, 24 * cMB, 12, 0, c256MiB, c32MiB, 0),          //
          D_ALLOC_CONFIG(44, 32 * cMB, 12, 0, c256MiB, c32MiB, 0),          //
          D_ALLOC_CONFIG(45, 48 * cMB, 13, 0, c256MiB, c64MiB, 0),          //
          D_ALLOC_CONFIG(46, 64 * cMB, 13, 0, c256MiB, c64MiB, 0),          //
          D_ALLOC_CONFIG(47, 96 * cMB, 14, 0, c256MiB, c128MiB, 0),         //
          D_ALLOC_CONFIG(48, 128 * cMB, 14, 0, c256MiB, c128MiB, 0),        //
          D_ALLOC_CONFIG(49, 192 * cMB, 15, 0, c512MiB, c256MiB, 0),        //
          D_ALLOC_CONFIG(50, 256 * cMB, 15, 0, c512MiB, c256MiB, 0),        //
          D_ALLOC_CONFIG(51, 384 * cMB, 16, 0, c1GiB, c512MiB, 0),          //
          D_ALLOC_CONFIG(52, 512 * cMB, 16, 0, c1GiB, c512MiB, 0),          //
        };

        static const s32 cAllocConfigCount = DARRAYSIZE(s_alloc_configs);
        static const s32 cMinBin           = 6;
        static const s32 cMaxBin           = cMinBin + cAllocConfigCount - 1;
#endif

        struct arena_pool_t
        {
            u8             m_id;               // identifier for the arena pool (e.g., type or category)
            u8             m_reserved;         // reserved for future use or alignment
            u16            m_reserved2;        // reserved for future use or alignment
            u32            m_allocation_size;  // size of each allocation within the arena
            arena_offset_t m_free_list;        // head of the free list within the arena
            u32            m_memory_size;      // Size of the arena memory for allocating regions/chunks/blocks
            byte*          m_memory_base;      // For allocating regions/chunks/blocks
            byte*          m_memory_cursor;    // Current cursor position within the arena memory (aligned to 8 bytes)
        };

        // An allocator consists of N regions
        // Example size of an allocator: 128 GB or 256 GB
        struct allocator_t
        {
            arena_offset_t* m_active_chunks_per_alloc_index;        // array of active chunks per allocation index
            arena_offset_t* m_cached_empty_chunks_per_chunk_index;  // array of cached empty chunks per chunk index
            arena_offset_t* m_cached_empty_blocks_per_block_index;  // array of cached empty blocks per block index
            arena_offset_t* m_active_regions_per_region_size;       // array of active regions per region size
            arena_offset_t* m_unused_region_array;                  // free lists per region size, (nil = 0xffffffff), covers virtual memory range

            alloc_config_t* m_alloc_config_array;

            s8              m_min_region_sizeshift;   // shift value to calculate minimum region size
            arena_offset_t* m_o_region_lookup_array;  // covers virtual memory range

            byte* m_memory_base;  // virtual memory base for the allocator (must be aligned to region size)
            u64   m_memory_size;  // size of the virtual memory address range for this allocator

            arena_pool_t m_meta_arena;       // metadata arena (chunk_t/block_t/region_t)
            arena_pool_t m_array_arenas[6];  // array arenas (for different array sizes, 8/16/32/64/128/256)
        };

        static inline u32       s_region_to_offset(allocator_t* a, region_t* region) { return (u32)((byte*)region - a->m_meta_arena.m_memory_base); }
        static inline region_t* s_offset_to_region(allocator_t* a, u32 offset) { return (region_t*)(a->m_meta_arena.m_memory_base + offset); }
        static inline u32       s_chunk_to_offset(allocator_t* a, chunk_t* chunk) { return (u32)((byte*)chunk - a->m_meta_arena.m_memory_base); }
        static inline chunk_t*  s_offset_to_chunk(allocator_t* a, u32 offset) { return (chunk_t*)(a->m_meta_arena.m_memory_base + offset); }
        static inline u32       s_block_to_offset(allocator_t* a, block_t* block) { return (u32)((byte*)block - a->m_meta_arena.m_memory_base); }
        static inline block_t*  s_offset_to_block(allocator_t* a, u32 offset) { return (block_t*)(a->m_meta_arena.m_memory_base + offset); }

#ifdef CSUPERALLOC_V3_D2
        static s32 s_size_to_bin(u32& alloc_size)
        {
            const s32 w   = math::countLeadingZeros(alloc_size);
            const u32 f   = (u32)0x80000000 >> w;
            const u32 r   = 0xFFFFFFFF << (30 - w);
            const u32 t   = ((f - 1) >> 1);
            alloc_size    = (alloc_size + t) & ~t;
            const s32 bin = (s32)((alloc_size & r) >> (30 - w)) + ((30 - w) * 2);
            return bin;
        }
#endif

#ifdef CSUPERALLOC_V3_D4
        static s32 s_size_to_bin(u32& alloc_size)
        {
            const s32 w   = math::countLeadingZeros(alloc_size);
            const u32 f   = (u32)0x80000000 >> w;
            const u32 r   = 0xFFFFFFFF << (29 - w);
            const u32 t   = ((f - 1) >> 2);
            alloc_size    = (alloc_size + t) & ~t;
            const s32 bin = (s32)((alloc_size & r) >> (29 - w)) + ((29 - w) * 4);
            return bin;
        }
#endif

        static alloc_config_t* s_get_alloc_config_for_size(allocator_t* a, u32 alloc_size)
        {
            // Between every power-of-2 and the next power-of-2 we want D sizes.
            // If D is 2, then we want the following:
            //    - step = (next power-of-2 - current power-of-2) / 2
            //    - sizes = current-power-of-2, current-power-of-2 + step
            // If D is 4, then we want the following:
            //    - step = (next power-of-2 - current power-of-2) / 4
            //    - sizes = current-power-of-2, current-power-of-2 + step, current-power-of-2 + 2 * step, current-power-of-2 + 3 * step

            // Additional requirements:
            // - Any size below 8 should act as 'size = 8'
            // - Every size should be a multiple of 8

            ASSERT(alloc_size > 0);
            alloc_size = ((alloc_size + 7) / 8) * 8;
            s32 bin    = s_size_to_bin(alloc_size);
            if (bin > cMaxBin)
                return nullptr;
            if (bin < cMinBin)
                bin = cMinBin;
            alloc_config_t* config = &s_alloc_configs[bin - cMinBin];
            ASSERT(alloc_size <= config->m_alloc_size);
            return config;
        }

        void* allocate(allocator_t* a, u32 size)
        {
            // - alloc_config_t* ac = get_alloc_config_for_size(a, size);
            // - Is there an active chunk for this allocation config
            //   - If not, check if there is an active region for this allocation config
            //     - If not, allocate a new region for the region configuration
            //     - If yes, allocate a new chunk within the active region, add it to the active chunks
            //   - Allocate item from the active chunk
            //   - If chunk is now full remove it from the active list
            // - return item
        }

        void deallocate(allocator_t* a, void* item)
        {
            // - Determine region the item belongs to, using allocator->m_o_region_lookup_array[item_address >> region_shift]
            // - If region is chunk region find the chunk holding item
            //   else find the block holding item
            // - Mark the item as free within the chunk, for a block we can add the element of the
            //   region block array to a list and add the block structure to the free list.
            // - If the chunk is now empty, release the bitvec array and tags array to the appropriate array free list.
            //   Add the chunk structure to the free list.
            //   Add the region chunk array slot to the free-list in the region
            // - If the chunk was full before this deallocation, add it back to the active list
        }

    }  // namespace nsuperallocv3

}  // namespace ncore
