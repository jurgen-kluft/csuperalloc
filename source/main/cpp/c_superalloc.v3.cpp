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
        // Note:
        // Thought, it seems a region of 16MB with Chunks of 64KB could both use
        // chunks and blocks, nothing is preventing a region from being used in
        // both ways, right? The only thing we need is to know the type when picking
        // an item from the array in the region.

#define D_CSUPERALLOC_V3_D2

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

        // A chunk based region consists of N chunks
        // A chunk consists of N elements with a maximum of 4096
        // Size: 32 bytes (must be <= 32 bytes)
        struct chunk_t
        {
            u8  m_type;             // 0, chunk type (e.g., fixed-size chunk)
            u8  m_alloc_cfg_index;  // allocation configuration index for this chunk
            u16 m_index;            // index into region->m_o_array
            u16 m_count;            // cur number of elements
            u16 m_free_item_index;  // index to first virgin item in the chunk
            u64 m_bin0;             // bitvec level 0
            u32 m_o_bin1;           // bitvec level 1 (offset into arena memory to u64[])
            u32 m_o_tags;           // array of tags (offset into arena memory to a array_t)
            u32 m_o_region;         // region this chunk belongs to (offset into arena memory to a region_t)
            u32 m_o_next;           // next chunk in list (offset into arena memory to a chunk_t)
        };

        // A block based region consists of N blocks
        // A block is a fixed size number of pages (power-of-two number of pages).
        // But an allocation can use less pages than the block size, so we need to track
        // the number of committed pages per block.
        // Size: 24 bytes (must be <= 32 bytes)
        struct block_t
        {
            u8  m_type;             // 1, block type (e.g., fixed-size block)
            u8  m_alloc_cfg_index;  // allocation configuration index for this block
            u16 m_reserved2;        // reserved for future use
            u32 m_pages;            // number of committed pages in the block
            u32 m_o_region;         // region this block belongs to (offset into arena memory to a region_t)
            u32 m_index;            // index into region->m_o_array
            u32 m_tag;              // tag for the block (e.g., for tracking usage or type)
            u32 m_o_next;           // next block in list (offset into arena memory to a block_t)
        };

        // A region consists of N chunks/blocks
        // Example size of a region: 16 MB
        // Note: A region is locked to a specific region configuration
        // Size: 24 bytes (must be <= 32 bytes)
        struct region_t
        {
            u8  m_type;             // 2, region type (e.g., chunk-based or block-based)
            u8  m_alloc_cfg_index;  // allocation configuration index for this region
            u16 m_reserved2;        // reserved for future use
            u32 m_index;            // index into allocator->m_o_region_lookup_array
            u32 m_free_slot_index;  // index of the first virgin chunk/block slot in m_array
            u32 m_free_list;        // index of first slot of the free list for chunks/blocks
            u32 m_o_array;          // array of chunk/block offsets (offset into arena memory to a array_t)
            u32 m_o_next;           // next region in list (offset into arena memory)
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

#ifdef D_CSUPERALLOC_V3_D2
        // D == 2
        static alloc_config_t s_alloc_configs[] = {
          D_ALLOC_CONFIG(0, 8, 0, c16MiB, 0, c16KiB, 11),                    // bin1, u64[32], 256 bytes
          D_ALLOC_CONFIG(2, 12, 0, c16MiB, 1, c16KiB, 10),                   // bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(2, 16, 0, c16MiB, 1, c16KiB, 10),                   // bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(3, 24, 1, c16MiB, 1, c16KiB, 10),                   // bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(4, 32, 1, c16MiB, 2, c32KiB, 10),                   // bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(5, 48, 1, c16MiB, 2, c32KiB, 10),                   // bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(6, 64, 2, c16MiB, 3, c64KiB, 10),                   // bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(7, 96, 2, c16MiB, 3, c64KiB, 10),                   // bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(8, 128, 2, c16MiB, 4, c64KiB, 9),                   // bin1, u64[8], 64 bytes
          D_ALLOC_CONFIG(9, 192, 2, c16MiB, 4, c64KiB, 9),                   // bin1, u64[8], 64 bytes
          D_ALLOC_CONFIG(10, 256, 2, c16MiB, 5, c64KiB, 8),                  // bin1, u64[4], 32 bytes
          D_ALLOC_CONFIG(11, 384, 2, c16MiB, 5, c64KiB, 8),                  // bin1, u64[4], 32 bytes
          D_ALLOC_CONFIG(12, 512, 2, c16MiB, 6, c64KiB, 7),                  // bin1, u64[2], 16 bytes
          D_ALLOC_CONFIG(13, 768, 2, c16MiB, 6, c64KiB, 7),                  // bin1, u64[2], 16 bytes
          D_ALLOC_CONFIG(14, 1 * cKB, 2, c16MiB, 7, c64KiB, 6),              // bin1 is unused
          D_ALLOC_CONFIG(15, 1 * cKB + 512, 2, c16MiB, 7, c64KiB, 6),        // bin1 is unused
          D_ALLOC_CONFIG(16, 2 * cKB, 2, c16MiB, 8, c64KiB, 5),              // bin1 is unused
          D_ALLOC_CONFIG(17, 3 * cKB, 2, c16MiB, 8, c64KiB, 5),              // bin1 is unused
          D_ALLOC_CONFIG(18, 4 * cKB, 2, c16MiB, 9, c64KiB, 4),              // bin1 is unused
          D_ALLOC_CONFIG(19, 6 * cKB, 2, c16MiB, 9, c64KiB, 4),              // bin1 is unused
          D_ALLOC_CONFIG(20, 8 * cKB, 2, c16MiB, 9, c64KiB, 3),              // bin1 is unused
          D_ALLOC_CONFIG(21, 12 * cKB, 3, c16MiB, 10, c16KiB, 0),            // block based region
          D_ALLOC_CONFIG(22, 16 * cKB, 3, c16MiB, 10, c16KiB, 0),            // block based region
          D_ALLOC_CONFIG(23, 24 * cKB, 3, c16MiB, 11, c32KiB, 0),            // block based region
          D_ALLOC_CONFIG(24, 32 * cKB, 3, c16MiB, 11, c32KiB, 0),            // block based region
          D_ALLOC_CONFIG(25, 48 * cKB, 3, c16MiB, 12, c64KiB, 0),            // block based region
          D_ALLOC_CONFIG(26, 64 * cKB, 3, c16MiB, 12, c64KiB, 0),            // block based region
          D_ALLOC_CONFIG(27, 96 * cKB, 4, c16MiB, 13, c128KiB, 0),           // block based region
          D_ALLOC_CONFIG(28, 128 * cKB, 4, c16MiB, 13, c128KiB, 0),          // block based region
          D_ALLOC_CONFIG(29, 192 * cKB, 5, c16MiB, 14, c256KiB, 0),          // block based region
          D_ALLOC_CONFIG(30, 256 * cKB, 5, c16MiB, 14, c256KiB, 0),          // block based region
          D_ALLOC_CONFIG(31, 384 * cKB, 6, c16MiB, 15, c512KiB, 0),          // block based region
          D_ALLOC_CONFIG(32, 512 * cKB, 6, c16MiB, 15, c512KiB, 0),          // block based region
          D_ALLOC_CONFIG(33, 768 * cKB, 7, c32MiB, 16, c1MiB, 0),            // block based region
          D_ALLOC_CONFIG(34, 1 * cMB, 7, c32MiB, 16, c1MiB, 0),              // block based region
          D_ALLOC_CONFIG(35, 1 * cMB + 512 * cKB, 8, c32MiB, 17, c2MiB, 0),  // block based region
          D_ALLOC_CONFIG(36, 2 * cMB, 8, c32MiB, 17, c2MiB, 0),              // block based region
          D_ALLOC_CONFIG(37, 3 * cMB, 9, c32MiB, 18, c4MiB, 0),              // block based region
          D_ALLOC_CONFIG(38, 4 * cMB, 9, c32MiB, 18, c4MiB, 0),              // block based region
          D_ALLOC_CONFIG(39, 6 * cMB, 10, c32MiB, 19, c8MiB, 0),             // block based region
          D_ALLOC_CONFIG(40, 8 * cMB, 10, c32MiB, 19, c8MiB, 0),             // block based region
          D_ALLOC_CONFIG(41, 12 * cMB, 11, c256MiB, 20, c16MiB, 0),          // block based region
          D_ALLOC_CONFIG(42, 16 * cMB, 11, c256MiB, 20, c16MiB, 0),          // block based region
          D_ALLOC_CONFIG(43, 24 * cMB, 12, c256MiB, 21, c32MiB, 0),          // block based region
          D_ALLOC_CONFIG(44, 32 * cMB, 12, c256MiB, 21, c32MiB, 0),          // block based region
          D_ALLOC_CONFIG(45, 48 * cMB, 13, c256MiB, 22, c64MiB, 0),          // block based region
          D_ALLOC_CONFIG(46, 64 * cMB, 13, c256MiB, 22, c64MiB, 0),          // block based region
          D_ALLOC_CONFIG(47, 96 * cMB, 14, c256MiB, 23, c128MiB, 0),         // block based region
          D_ALLOC_CONFIG(48, 128 * cMB, 14, c256MiB, 23, c128MiB, 0),        // block based region
          D_ALLOC_CONFIG(49, 192 * cMB, 15, c512MiB, 24, c256MiB, 0),        // block based region
          D_ALLOC_CONFIG(50, 256 * cMB, 15, c512MiB, 24, c256MiB, 0),        // block based region
          D_ALLOC_CONFIG(51, 384 * cMB, 16, c1GiB, 25, c512MiB, 0),          // block based region
          D_ALLOC_CONFIG(52, 512 * cMB, 16, c1GiB, 25, c512MiB, 0),          // block based region
        };

        static const s32 cAllocConfigCount = DARRAYSIZE(s_alloc_configs);
        static const s32 cMinBin           = 6;
        static const s32 cMaxBin           = cMinBin + cAllocConfigCount - 1;
#endif

        struct arena_pool_t
        {
            u8    m_reserved;         // reserved for future use
            u8    m_page_sizeshift;   // shift value to calculate the page size within the arena
            u16   m_allocation_size;  // size of each allocation within the arena
            u32   m_free_list;        // head of the free list within the arena
            u32   m_committed_pages;  // number of current committed pages within the arena
            u32   m_reserved_pages;   // number of reserved pages within the arena
            byte* m_memory_base;      // For allocating regions/chunks/blocks
            byte* m_memory_cursor;    // Current cursor position within the arena memory (aligned to 8 bytes)
        };

        static void* s_allocate(arena_pool_t* arena)
        {
            if (arena->m_free_list == 0xffffffff)
            {
                void* ptr = arena->m_memory_cursor;

                arena->m_memory_cursor += arena->m_allocation_size;

                const u32 current_page = (arena->m_memory_cursor - arena->m_memory_base) >> arena->m_page_sizeshift;

                // check if we have enough committed pages
                if (current_page > arena->m_committed_pages)
                {
                    ASSERT(current_page <= arena->m_reserved_pages);
                    // TODO commit more pages
                    arena->m_committed_pages = current_page;
                }

                return ptr;
            }
            else
            {
                void* ptr          = arena->m_memory_base + arena->m_free_list;
                arena->m_free_list = *((u32*)ptr);
                return ptr;
            }
        }

        static void s_free(arena_pool_t* arena, void* ptr)
        {
            ASSERT(((uint_t)ptr & 0x7) == 0);  // Ensure the pointer is 8-byte aligned
            *((u32*)ptr)       = arena->m_free_list;
            arena->m_free_list = (u32)((byte*)ptr - arena->m_memory_base);
        }

        // An allocator consists of N regions
        // Example size of an allocator: 128 GB or 256 GB
        struct allocator_t
        {
            u32* m_not_full_o_chunks_per_alloc_index;      // array of "not full" chunk list per allocation index
            u32* m_cached_empty_o_chunks_per_chunk_index;  // array of cached empty chunks/blocks per chunk index
            u32* m_active_o_region_per_region_index;       // array of active regions per region index
            u32* m_unused_o_region_per_region_index;       // free lists per region index

            alloc_config_t* m_alloc_config_array;

            s8   m_min_region_sizeshift;   // shift value to calculate minimum region size
            s8   m_arena_size_sizeshift;   // m_arena_memory_size / 8 (shift value to calculate arena size)
            u32* m_o_region_lookup_array;  // covers virtual memory range

            byte* m_memory_base;  // virtual memory base for the allocator (must be aligned to region size)
            u64   m_memory_size;  // size of the virtual memory address range for this allocator

            byte*        m_arena_memory_base;  // contiguous virtual memory base for all arenas
            u64          m_arena_memory_size;  // size of the contiguous virtual memory address range for arenas
            arena_pool_t m_arenas[12];         // array arenas (for meta-data, and array sizes (8/16/32/64/128/256/512/1024/2048/4096/8192))
        };

        static inline u8        s_pointer_to_arena_index(allocator_t* a, void* ptr) { return (u8)(((byte*)ptr - a->m_arena_memory_base) >> a->m_arena_size_sizeshift); }
        static inline u32       s_region_to_offset(allocator_t* a, region_t* region) { return (u32)((byte*)region - a->m_arena_memory_base); }
        static inline region_t* s_offset_to_region(allocator_t* a, u32 offset) { return (region_t*)(a->m_arena_memory_base + offset); }
        static inline u32       s_chunk_to_offset(allocator_t* a, chunk_t* chunk) { return (u32)((byte*)chunk - a->m_arena_memory_base); }
        static inline chunk_t*  s_offset_to_chunk(allocator_t* a, u32 offset) { return (chunk_t*)(a->m_arena_memory_base + offset); }
        static inline u32       s_block_to_offset(allocator_t* a, block_t* block) { return (u32)((byte*)block - a->m_arena_memory_base); }
        static inline block_t*  s_offset_to_block(allocator_t* a, u32 offset) { return (block_t*)(a->m_arena_memory_base + offset); }

        static chunk_t*  s_allocate_chunk(allocator_t* a) { return (chunk_t*)s_allocate(&a->m_arenas[0]); }
        static block_t*  s_allocate_block(allocator_t* a) { return (block_t*)s_allocate(&a->m_arenas[0]); }
        static region_t* s_allocate_region(allocator_t* a) { return (region_t*)s_allocate(&a->m_arenas[0]); }

        static void* s_allocate_array(allocator_t* a, u32 size)
        {
            // 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
            ASSERT(size >= 8 && size <= 8192);
            switch (size)
            {
                case 8: return s_allocate(&a->m_arenas[1]);
                case 16: return s_allocate(&a->m_arenas[2]);
                case 32: return s_allocate(&a->m_arenas[3]);
                case 64: return s_allocate(&a->m_arenas[4]);
                case 128: return s_allocate(&a->m_arenas[5]);
                case 256: return s_allocate(&a->m_arenas[6]);
                case 512: return s_allocate(&a->m_arenas[7]);
                case 1024: return s_allocate(&a->m_arenas[8]);
                case 2048: return s_allocate(&a->m_arenas[9]);
                case 4096: return s_allocate(&a->m_arenas[10]);
                case 8192: return s_allocate(&a->m_arenas[11]);
                default: break;
            }
            return nullptr;
        }

        static void s_free_pointer(allocator_t* a, void* ptr)
        {
            ASSERT(ptr >= a->m_arena_memory_base && ptr < (a->m_arena_memory_base + a->m_arena_memory_size));
            const u8 arena_index = s_pointer_to_arena_index(a, ptr);
            ASSERT(arena_index < 12);
            s_free(&a->m_arenas[arena_index], ptr);
        }
        static void s_free_chunk(allocator_t* a, chunk_t* chunk) { s_free_pointer(a, (void*)chunk); }
        static void s_free_block(allocator_t* a, block_t* block) { s_free_pointer(a, (void*)block); }
        static void s_free_region(allocator_t* a, region_t* region) { s_free_pointer(a, (void*)region); }
        static void s_free_array(allocator_t* a, void* array) { s_free_pointer(a, array); }

#ifdef D_CSUPERALLOC_V3_D2
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

#ifdef D_CSUPERALLOC_V3_D4
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
            //     - If not, allocate and initialize a new region for the allocation configuration
            //     - If yes, allocate and initialize a new chunk within the active region, add it to the active chunks
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
            //   region block array to a list and free the block structure.
            // - If the chunk is now empty, release the bitvec array and tags array to the appropriate array free list.
            //   Free the chunk structure.
            //   Add the region chunk array slot to the free-list in the region
            // - If the chunk was full before this deallocation, add it back to the active list
        }

    }  // namespace nsuperallocv3

}  // namespace ncore
