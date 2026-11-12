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

        // A chunk based region consists of N chunks
        // A chunk consists of N elements with a maximum of 2048.
        // Note: Bitvector needs to be fully initialized, not lazily.
        struct chunk_t
        {
            u32 m_capacity : 12;    // max number of elements in the chunk
            u32 m_count : 12;       // cur number of elements in the chunk
            u32 m_alloc_index : 8;  // allocation info index for this chunk
            u32 m_region_index;     // region this chunk belongs to (index into allocator->m_region_array)
            u32 m_bin0;             // binvec, level 0 (5)
            u32 m_o_bin1;           // binvec, level 1 (6) (offset into arena memory to an array_t)
            u32 m_o_tags;           // tags for each element in the chunk (offset into arena memory to an array_t)
            u32 m_o_next;           // next chunk in list (offset into arena memory)
        };

        // A block based region consists of N blocks
        // A block is a fixed size number of pages (power-of-two number of pages).
        // But an allocation can use less pages than the block size, so we need to track
        // the number of committed pages per block.
        struct block_t
        {
            u32 m_pages : 24;       // number of committed pages in the block
            u32 m_alloc_index : 8;  // allocation info index for this block
            u32 m_region_index;     // region this block belongs to (index into allocator->m_region_array)
            u32 m_o_next;           // next block in list (offset into arena memory)
            u32 m_tag;              // tag for the block
        };

        typedef u32 region_config_t;

#define D_CHUNK_REGION_CONFIG(region_config_index, region_size, chunk_size) (((region_config_index) & 0xFF) | ((0 & 0x1) << 31) | (((region_size) & 0xFF) << 8) | (((chunk_size) & 0xFF) << 16))
#define D_BLOCK_REGION_CONFIG(region_config_index, region_size, chunk_size) (((region_config_index) & 0xFF) | ((1 & 0x1) << 31) | (((region_size) & 0xFF) << 8) | (((chunk_size) & 0xFF) << 16))

        static inline u8 region_index(region_config_t config) { return (config >> 0) & 0xFF; }
        static inline u8 region_type(region_config_t config) { return (config >> 31) & 0x1; }
        static inline u8 region_size_shift(region_config_t config) { return (config >> 8) & 0xFF; }
        static inline u8 chunk_size_shift(region_config_t config) { return (config >> 16) & 0xFF; }

        // A region consists of N chunks/blocks
        // Example size of a region: 16 MB
        // Chunks in a region are in the same size range
        // Note: A region is locked to a specific region configuration
        struct region_t
        {
            u32             m_o_next;           // next (active) region in list (offset into arena memory)
            u32             m_index;            // index into allocator->m_region_array
            region_config_t m_region_config;    // the type of the region (e.g., chunk-based or block-based)
            u16             m_free_slot_index;  // index of the first virgin chunk/block slot in m_array
            u32             m_free_list;        // head of the free list for chunks/blocks within the region
            u32             m_o_array;          // array of chunk/block offsets (offset into arena memory)
        };

        struct alloc_config_t
        {
            u16             m_reindex;        // can redirect to another allocation configuration if needed
            u16             m_bitvec_size;    // size of the bit vector for tracking allocations within the chunk
            u32             m_alloc_size;     // size of the allocation
            region_config_t m_region_config;  // region configuration
        };

#define D_ALLOC_CONFIG(reindex, alloc_size, bitvec_size, region_config) {(reindex), (bitvec_size), (alloc_size), (region_config)}

#ifdef CSUPERALLOC_V3_D2
        // D == 2
        static alloc_config_t s_alloc_configs[] = {
          D_ALLOC_CONFIG(0, 8, 2048, D_CHUNK_REGION_CONFIG(0, c8MiB, c16KiB)),                  //
          D_ALLOC_CONFIG(2, 12, 1024, D_CHUNK_REGION_CONFIG(0, c8MiB, c16KiB)),                 //
          D_ALLOC_CONFIG(2, 16, 1024, D_CHUNK_REGION_CONFIG(0, c8MiB, c16KiB)),                 //
          D_ALLOC_CONFIG(3, 24, 1024, D_CHUNK_REGION_CONFIG(1, c8MiB, c16KiB)),                 //
          D_ALLOC_CONFIG(4, 32, 1024, D_CHUNK_REGION_CONFIG(1, c8MiB, c32KiB)),                 //
          D_ALLOC_CONFIG(5, 48, 1024, D_CHUNK_REGION_CONFIG(1, c8MiB, c32KiB)),                 //
          D_ALLOC_CONFIG(6, 64, 1024, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),                 //
          D_ALLOC_CONFIG(7, 96, 1024, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),                 //
          D_ALLOC_CONFIG(8, 128, 512, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),                 //
          D_ALLOC_CONFIG(9, 192, 512, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),                 //
          D_ALLOC_CONFIG(10, 256, 256, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),                //
          D_ALLOC_CONFIG(11, 384, 256, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),                //
          D_ALLOC_CONFIG(12, 512, 128, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),                //
          D_ALLOC_CONFIG(13, 768, 128, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),                //
          D_ALLOC_CONFIG(14, 1 * cKB, 64, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(15, 1 * cKB + 512, 64, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),       //
          D_ALLOC_CONFIG(16, 2 * cKB, 32, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(17, 3 * cKB, 32, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(18, 4 * cKB, 16, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(19, 6 * cKB, 16, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(20, 8 * cKB, 16, D_CHUNK_REGION_CONFIG(2, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(21, 12 * cKB, 0, D_BLOCK_REGION_CONFIG(3, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(22, 16 * cKB, 0, D_BLOCK_REGION_CONFIG(3, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(23, 24 * cKB, 0, D_BLOCK_REGION_CONFIG(3, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(24, 32 * cKB, 0, D_BLOCK_REGION_CONFIG(3, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(25, 48 * cKB, 0, D_BLOCK_REGION_CONFIG(3, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(26, 64 * cKB, 0, D_BLOCK_REGION_CONFIG(3, c8MiB, c64KiB)),             //
          D_ALLOC_CONFIG(27, 96 * cKB, 0, D_BLOCK_REGION_CONFIG(4, c16MiB, c128KiB)),           //
          D_ALLOC_CONFIG(28, 128 * cKB, 0, D_BLOCK_REGION_CONFIG(4, c16MiB, c128KiB)),          //
          D_ALLOC_CONFIG(29, 192 * cKB, 0, D_BLOCK_REGION_CONFIG(5, c16MiB, c256KiB)),          //
          D_ALLOC_CONFIG(30, 256 * cKB, 0, D_BLOCK_REGION_CONFIG(5, c16MiB, c256KiB)),          //
          D_ALLOC_CONFIG(31, 384 * cKB, 0, D_BLOCK_REGION_CONFIG(6, c16MiB, c512KiB)),          //
          D_ALLOC_CONFIG(32, 512 * cKB, 0, D_BLOCK_REGION_CONFIG(6, c16MiB, c512KiB)),          //
          D_ALLOC_CONFIG(33, 768 * cKB, 0, D_BLOCK_REGION_CONFIG(7, c32MiB, c1MiB)),            //
          D_ALLOC_CONFIG(34, 1 * cMB, 0, D_BLOCK_REGION_CONFIG(7, c32MiB, c1MiB)),              //
          D_ALLOC_CONFIG(35, 1 * cMB + 512 * cKB, 0, D_BLOCK_REGION_CONFIG(8, c32MiB, c2MiB)),  //
          D_ALLOC_CONFIG(36, 2 * cMB, 0, D_BLOCK_REGION_CONFIG(8, c32MiB, c2MiB)),              //
          D_ALLOC_CONFIG(37, 3 * cMB, 0, D_BLOCK_REGION_CONFIG(9, c32MiB, c4MiB)),              //
          D_ALLOC_CONFIG(38, 4 * cMB, 0, D_BLOCK_REGION_CONFIG(9, c32MiB, c4MiB)),              //
          D_ALLOC_CONFIG(39, 6 * cMB, 0, D_BLOCK_REGION_CONFIG(10, c32MiB, c8MiB)),             //
          D_ALLOC_CONFIG(40, 8 * cMB, 0, D_BLOCK_REGION_CONFIG(10, c32MiB, c8MiB)),             //
          D_ALLOC_CONFIG(41, 12 * cMB, 0, D_BLOCK_REGION_CONFIG(11, c256MiB, c16MiB)),          //
          D_ALLOC_CONFIG(42, 16 * cMB, 0, D_BLOCK_REGION_CONFIG(11, c256MiB, c16MiB)),          //
          D_ALLOC_CONFIG(43, 24 * cMB, 0, D_BLOCK_REGION_CONFIG(12, c256MiB, c32MiB)),          //
          D_ALLOC_CONFIG(44, 32 * cMB, 0, D_BLOCK_REGION_CONFIG(12, c256MiB, c32MiB)),          //
          D_ALLOC_CONFIG(45, 48 * cMB, 0, D_BLOCK_REGION_CONFIG(13, c256MiB, c64MiB)),          //
          D_ALLOC_CONFIG(46, 64 * cMB, 0, D_BLOCK_REGION_CONFIG(13, c256MiB, c64MiB)),          //
          D_ALLOC_CONFIG(47, 96 * cMB, 0, D_BLOCK_REGION_CONFIG(14, c256MiB, c128MiB)),         //
          D_ALLOC_CONFIG(48, 128 * cMB, 0, D_BLOCK_REGION_CONFIG(14, c256MiB, c128MiB)),        //
          D_ALLOC_CONFIG(49, 192 * cMB, 0, D_BLOCK_REGION_CONFIG(15, c512MiB, c256MiB)),        //
          D_ALLOC_CONFIG(50, 256 * cMB, 0, D_BLOCK_REGION_CONFIG(15, c512MiB, c256MiB)),        //
          D_ALLOC_CONFIG(51, 384 * cMB, 0, D_BLOCK_REGION_CONFIG(16, c1GiB, c512MiB)),          //
          D_ALLOC_CONFIG(52, 512 * cMB, 0, D_BLOCK_REGION_CONFIG(16, c1GiB, c512MiB)),          //
        };

        static const s32 cAllocConfigCount = DARRAYSIZE(s_alloc_configs);
        static const s32 cMinBin           = 6;
        static const s32 cMaxBin           = cMinBin + cAllocConfigCount - 1;
#endif

        // An allocator consists of N regions
        // Example size of an allocator: 128 GB or 256 GB
        struct allocator_t
        {
            u32* m_active_chunks_per_alloc_index;
            u32* m_cached_empty_chunks_per_alloc_range;
            u32* m_cached_empty_blocks_per_alloc_range;
            u32* m_free_empty_chunks_per_alloc_range;
            u32* m_free_empty_blocks_per_alloc_range;
            u32* m_active_regions_per_region_index;
            u32* m_free_region_array;  // (nil = 0xffffffff), covers virtual memory range

            region_config_t* m_region_config_array;
            alloc_config_t*  m_alloc_config_array;

            u32* m_empty_arrays;  // array of list-heads for empty arrays (power-of-2 sizes)

            s8   m_min_region_sizeshift;   // shift value to calculate minimum region size
            u32* m_o_region_lookup_array;  // (nil = 0xffffffff), covers virtual memory range

            byte* m_memory_base;  // Virtual memory base for the allocator (aligned to region size)
            u64   m_memory_size;  // Size of the virtual memory for the allocator

            byte* m_arena_memory_base;  // For allocating regions/chunks/blocks
            byte* m_arena_memory_cursor;
            u64   m_arena_memory_size;  // Size of the arena memory for allocating regions/chunks/blocks
        };

        static inline u32       s_region_to_offset(allocator_t* a, region_t* region) { return (u32)((byte*)region - a->m_arena_memory_base); }
        static inline region_t* s_offset_to_region(allocator_t* a, u32 offset) { return (region_t*)(a->m_arena_memory_base + offset); }
        static inline u32       s_chunk_to_offset(allocator_t* a, chunk_t* chunk) { return (u32)((byte*)chunk - a->m_arena_memory_base); }
        static inline chunk_t*  s_offset_to_chunk(allocator_t* a, u32 offset) { return (chunk_t*)(a->m_arena_memory_base + offset); }
        static inline u32       s_block_to_offset(allocator_t* a, block_t* block) { return (u32)((byte*)block - a->m_arena_memory_base); }
        static inline block_t*  s_offset_to_block(allocator_t* a, u32 offset) { return (block_t*)(a->m_arena_memory_base + offset); }

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
