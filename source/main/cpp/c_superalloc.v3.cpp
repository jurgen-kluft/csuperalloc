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
            u32 m_o_bin1;           // binvec, level 1 (6) (offset into arena memory to an array_t)
            u32 m_bin0;             // binvec, level 0 (5)
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

        // struct region_config_t
        // {
        //     u8 m_region_type;        // chunk or block based
        //     u8 m_region_size_shift;  // region size (1 << region_size_shift)
        //     u8 m_chunk_size_shift;   // chunk/block size shift
        // };

        enum
        {
            cRegionTypeChunk = 0,
            cRegionTypeBlock = 1,
        };

        typedef u16 region_config_t;

#define REGION_CONFIG(type, region_size, chunk_size) (((type) & 0x1) | (((region_size) & 0x3F) << 1) | (((chunk_size) & 0x3F) << (1 + 6)))

        static inline u8 region_type(region_config_t config) { return (config >> 0) & 0x1; }
        static inline u8 region_size_shift(region_config_t config) { return (config >> 1) & 0x3F; }
        static inline u8 chunk_size_shift(region_config_t config) { return (config >> 7) & 0x3F; }

        // static region_config_t s_regions[] = {
        //   REGION_CONFIG(cRegionTypeChunk, c16MiB, c16KiB),    //
        //   REGION_CONFIG(cRegionTypeChunk, c16MiB, c32KiB),    //
        //   REGION_CONFIG(cRegionTypeChunk, c16MiB, c64KiB),    //
        //   REGION_CONFIG(cRegionTypeBlock, c32MiB, c64KiB),    //
        //   REGION_CONFIG(cRegionTypeBlock, c32MiB, c128KiB),   //
        //   REGION_CONFIG(cRegionTypeBlock, c32MiB, c256KiB),   //
        //   REGION_CONFIG(cRegionTypeBlock, c32MiB, c512KiB),   //
        //   REGION_CONFIG(cRegionTypeBlock, c32MiB, c1MiB),     //
        //   REGION_CONFIG(cRegionTypeBlock, c64MiB, c2MiB),     //
        //   REGION_CONFIG(cRegionTypeBlock, c64MiB, c4MiB),     //
        //   REGION_CONFIG(cRegionTypeBlock, c64MiB, c8MiB),     //
        //   REGION_CONFIG(cRegionTypeBlock, c512MiB, c16MiB),   //
        //   REGION_CONFIG(cRegionTypeBlock, c512MiB, c32MiB),   //
        //   REGION_CONFIG(cRegionTypeBlock, c512MiB, c64MiB),   //
        //   REGION_CONFIG(cRegionTypeBlock, c512MiB, c128MiB),  //
        //   REGION_CONFIG(cRegionTypeBlock, c1GiB, c256MiB),    //
        //   REGION_CONFIG(cRegionTypeBlock, c1GiB, c512MiB),    //
        //   REGION_CONFIG(cRegionTypeBlock, c2GiB, c1GiB),      //
        // };

        // A region consists of N chunks/blocks
        // Example size of a region: 16 MB
        // Chunks in a region are in the same size range
        // A region is thus dedicated to a specific alloc range
        struct region_t
        {
            u32             m_o_next;           // next region in list (offset into arena memory)
            u32             m_index;            // index into allocator->m_region_array
            region_config_t m_region_config;    // the type of the region (e.g., chunk-based or block-based)
            u16             m_free_slot_index;  // index of the first virgin chunk/block slot in m_array
            u32             m_o_array;          // array of chunk/block offsets (offset into arena memory)
        };

        struct alloc_config_t
        {
            u32 m_alloc_size;     // size of the allocation
            u16 m_region_config;  // index into region configuration array
        };

#define ALLOC_CONFIG(size, config) {(size), (config)}

        static const region_config_t cChunk16KiB        = REGION_CONFIG(cRegionTypeChunk, c8MiB, c16KiB);
        static const region_config_t cChunk32KiB        = REGION_CONFIG(cRegionTypeChunk, c8MiB, c32KiB);
        static const region_config_t cChunk64KiB        = REGION_CONFIG(cRegionTypeChunk, c8MiB, c64KiB);
        static const region_config_t cBlock8MiB64KiB    = REGION_CONFIG(cRegionTypeBlock, c8MiB, c64KiB);
        static const region_config_t cBlock32MiB64KiB   = REGION_CONFIG(cRegionTypeBlock, c32MiB, c64KiB);
        static const region_config_t cBlock32MiB128KiB  = REGION_CONFIG(cRegionTypeBlock, c32MiB, c128KiB);
        static const region_config_t cBlock32MiB256KiB  = REGION_CONFIG(cRegionTypeBlock, c32MiB, c256KiB);
        static const region_config_t cBlock32MiB512KiB  = REGION_CONFIG(cRegionTypeBlock, c32MiB, c512KiB);
        static const region_config_t cBlock32MiB1MiB    = REGION_CONFIG(cRegionTypeBlock, c32MiB, c1MiB);
        static const region_config_t cBlock32MiB2MiB    = REGION_CONFIG(cRegionTypeBlock, c32MiB, c2MiB);
        static const region_config_t cBlock32MiB4MiB    = REGION_CONFIG(cRegionTypeBlock, c32MiB, c4MiB);
        static const region_config_t cBlock32MiB8MiB    = REGION_CONFIG(cRegionTypeBlock, c32MiB, c8MiB);
        static const region_config_t cBlock256MiB16MiB  = REGION_CONFIG(cRegionTypeBlock, c256MiB, c16MiB);
        static const region_config_t cBlock256MiB32MiB  = REGION_CONFIG(cRegionTypeBlock, c256MiB, c32MiB);
        static const region_config_t cBlock256MiB64MiB  = REGION_CONFIG(cRegionTypeBlock, c256MiB, c64MiB);
        static const region_config_t cBlock256MiB128MiB = REGION_CONFIG(cRegionTypeBlock, c256MiB, c128MiB);
        static const region_config_t cBlock512MiB256MiB = REGION_CONFIG(cRegionTypeBlock, c512MiB, c256MiB);
        static const region_config_t cBlock1GiB512MiB   = REGION_CONFIG(cRegionTypeBlock, c1GiB, c512MiB);

#ifdef CSUPERALLOC_V3_D2

        static const s32 cMinBin = 6;

        // D == 2
        static alloc_config_t s_alloc_configs[] = {
          ALLOC_CONFIG(8, cChunk16KiB),
          ALLOC_CONFIG(16, cChunk16KiB),
          ALLOC_CONFIG(16, cChunk16KiB),
          ALLOC_CONFIG(24, cChunk32KiB),
          ALLOC_CONFIG(32, cChunk32KiB),
          ALLOC_CONFIG(48, cChunk32KiB),
          ALLOC_CONFIG(64, cChunk64KiB),
          ALLOC_CONFIG(96, cChunk64KiB),
          ALLOC_CONFIG(128, cChunk64KiB),
          ALLOC_CONFIG(192, cChunk64KiB),
          ALLOC_CONFIG(256, cChunk64KiB),
          ALLOC_CONFIG(384, cChunk64KiB),
          ALLOC_CONFIG(512, cChunk64KiB),
          ALLOC_CONFIG(768, cChunk64KiB),
          ALLOC_CONFIG(1 * cKB, cChunk64KiB),
          ALLOC_CONFIG(1 * cKB + 512, cChunk64KiB),
          ALLOC_CONFIG(2 * cKB, cChunk64KiB),
          ALLOC_CONFIG(3 * cKB, cChunk64KiB),
          ALLOC_CONFIG(4 * cKB, cChunk64KiB),
          ALLOC_CONFIG(6 * cKB, cChunk64KiB),
          ALLOC_CONFIG(8 * cKB, cChunk64KiB),
          ALLOC_CONFIG(12 * cKB, cBlock8MiB64KiB),
          ALLOC_CONFIG(16 * cKB, cBlock8MiB64KiB),
          ALLOC_CONFIG(24 * cKB, cBlock8MiB64KiB),
          ALLOC_CONFIG(32 * cKB, cBlock8MiB64KiB),
          ALLOC_CONFIG(48 * cKB, cBlock32MiB64KiB),
          ALLOC_CONFIG(64 * cKB, cBlock32MiB64KiB),
          ALLOC_CONFIG(96 * cKB, cBlock32MiB128KiB),
          ALLOC_CONFIG(128 * cKB, cBlock32MiB128KiB),
          ALLOC_CONFIG(192 * cKB, cBlock32MiB256KiB),
          ALLOC_CONFIG(256 * cKB, cBlock32MiB256KiB),
          ALLOC_CONFIG(384 * cKB, cBlock32MiB512KiB),
          ALLOC_CONFIG(512 * cKB, cBlock32MiB512KiB),
          ALLOC_CONFIG(768 * cKB, cBlock32MiB1MiB),
          ALLOC_CONFIG(1 * cMB, cBlock32MiB1MiB),
          ALLOC_CONFIG(1 * cMB + 512 * cKB, cBlock32MiB2MiB),
          ALLOC_CONFIG(2 * cMB, cBlock32MiB2MiB),
          ALLOC_CONFIG(3 * cMB, cBlock32MiB4MiB),
          ALLOC_CONFIG(4 * cMB, cBlock32MiB4MiB),
          ALLOC_CONFIG(6 * cMB, cBlock32MiB8MiB),
          ALLOC_CONFIG(8 * cMB, cBlock32MiB8MiB),
          ALLOC_CONFIG(12 * cMB, cBlock256MiB16MiB),
          ALLOC_CONFIG(16 * cMB, cBlock256MiB16MiB),
          ALLOC_CONFIG(24 * cMB, cBlock256MiB32MiB),
          ALLOC_CONFIG(32 * cMB, cBlock256MiB32MiB),
          ALLOC_CONFIG(48 * cMB, cBlock256MiB64MiB),
          ALLOC_CONFIG(64 * cMB, cBlock256MiB64MiB),
          ALLOC_CONFIG(96 * cMB, cBlock256MiB128MiB),
          ALLOC_CONFIG(128 * cMB, cBlock256MiB128MiB),
          ALLOC_CONFIG(192 * cMB, cBlock512MiB256MiB),
          ALLOC_CONFIG(256 * cMB, cBlock512MiB256MiB),
          ALLOC_CONFIG(384 * cMB, cBlock1GiB512MiB),
          ALLOC_CONFIG(512 * cMB, cBlock1GiB512MiB),
        };

        static const s32 cAllocConfigCount = DARRAYSIZE(s_alloc_configs);
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
            u32* m_active_regions_per_alloc_range;

            region_config_t* m_region_config_array;
            alloc_config_t*  m_alloc_config_array;

            s8 m_region_sizeshift;  // shift value to calculate region size

            byte* m_memory_base;     // Virtual memory base for the allocator (aligned to region size)
            u64   m_memory_size;     // Size of the virtual memory for the allocator
            u32*  m_o_region_array;  // (nil = 0xffffffff), covers virtual memory range

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
            // - Determine region the item belongs to
            // - If region is chunk region find the chunk c ontaining the item
            //   else find the block containing the item
            // - Mark the item as free within the chunk or block
            // - If the chunk or block is now empty, move it to the cached or free list
            // - If the chunk was full before this deallocation, add it back to the active list
        }

    }  // namespace nsuperallocv3

}  // namespace ncore
