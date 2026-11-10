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

        typedef u32 region_config_t;

#define REGION_CONFIG(type, region_size, chunk_size) (((type) & 0xFF) | (((region_size) & 0xFF) << 8) | (((chunk_size) & 0xFF) << 16))

        static inline u8 region_type(region_config_t config) { return (config >> 0) & 0xFF; }
        static inline u8 region_size_shift(region_config_t config) { return (config >> 8) & 0xFF; }
        static inline u8 chunk_size_shift(region_config_t config) { return (config >> 16) & 0xFF; }

        static region_config_t s_regions[] = {
          REGION_CONFIG(cRegionTypeChunk, c16MiB, c16KiB),    //
          REGION_CONFIG(cRegionTypeChunk, c16MiB, c32KiB),    //
          REGION_CONFIG(cRegionTypeChunk, c16MiB, c64KiB),    //
          REGION_CONFIG(cRegionTypeBlock, c32MiB, c64KiB),    //
          REGION_CONFIG(cRegionTypeBlock, c32MiB, c128KiB),   //
          REGION_CONFIG(cRegionTypeBlock, c32MiB, c256KiB),   //
          REGION_CONFIG(cRegionTypeBlock, c32MiB, c512KiB),   //
          REGION_CONFIG(cRegionTypeBlock, c32MiB, c1MiB),     //
          REGION_CONFIG(cRegionTypeBlock, c64MiB, c2MiB),     //
          REGION_CONFIG(cRegionTypeBlock, c64MiB, c4MiB),     //
          REGION_CONFIG(cRegionTypeBlock, c64MiB, c8MiB),     //
          REGION_CONFIG(cRegionTypeBlock, c512MiB, c16MiB),   //
          REGION_CONFIG(cRegionTypeBlock, c512MiB, c32MiB),   //
          REGION_CONFIG(cRegionTypeBlock, c512MiB, c64MiB),   //
          REGION_CONFIG(cRegionTypeBlock, c512MiB, c128MiB),  //
          REGION_CONFIG(cRegionTypeBlock, c1GiB, c256MiB),    //
          REGION_CONFIG(cRegionTypeBlock, c1GiB, c512MiB),    //
          REGION_CONFIG(cRegionTypeBlock, c2GiB, c1GiB),      //
        };

        struct alloc_config_t
        {
            u32 m_alloc_size;           // size of the allocation
            u8  m_alloc_range_index;    // index of the allocation range
            u8  m_alloc_range_min;      // index to the minimum allocation config
            u8  m_alloc_range_max;      // index to the maximum allocation config
            u8  m_region_config_index;  // index into region configuration array
        };

        // Min
        static alloc_config_t s_alloc_configs[] = {

        };

    }  // namespace nsuperallocv3

}  // namespace ncore
