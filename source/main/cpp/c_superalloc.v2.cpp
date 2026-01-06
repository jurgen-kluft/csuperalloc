#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_binmap1.h"
#include "ccore/c_debug.h"
#include "ccore/c_limits.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"
#include "ccore/c_arena.h"

#include "csuperalloc/private/c_items.h"
#include "csuperalloc/private/c_list.h"

namespace ncore
{
    namespace nsuperallocv2
    {
        // N region struct exist in a continues separate address space with the following size:
        // Number-Of-Regions * N MiB. So each region has N MiB of address space to use for
        // its own management.
        // Each region starts at X * N MiB, where X is the region index (0,1,2,3,...)
        // The amount of committed pages for a region depends on the configuration of that region.
        // Example:
        // Region-Size in the main address space = 4 GiB
        // Chunk-Size = 64 KiB
        // Number of chunks for region to manage = 4 GiB / 64 KiB = 65536 chunks
        // sizeof(chunk_t) = 32 bytes * 65536 = 2.0 MiB for chunk array
        // However we will not fully commit the full chunk array, but will commit page by page on demand.
        // On MacOS, page-size = 16 KiB, so with one page we can already track 16 KiB / 32 B = 512 chunks.
        // The first committed page starts with struct region_t and followed by the chunk or block array.
        // sizeof(region_t) = 64 bytes
        //
        // When a region is using blocks, it is obvious that there will not be many block and so we will not
        // even use most of the first committed page.
        // We will accept this 'waste' for the benefit of simplicity, also there are not many regions to
        // begin with and even less that are 'block' based. In a large address space of 256 GiB we have
        // 256GiB/4GiB = 64 regions.
        //
        // The above setup makes the whole management of regions and chunks/blocks a lot simpler.

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
        };

#define D_MAX_ELEMENTS_PER_CHUNK 4096

        // A chunk consists of N elements with a maximum of 4096.
        // sizeof(chunk_t) = 32 bytes
        struct chunk_t
        {
            u16  m_next;                // next/prev for linked list
            u16  m_prev;                // next/prev for linked list
            u16  m_region_index;        // index into region array (back reference)
            u8   m_bin_index;           // bin index this chunk is used for
            u8   m_padding;             // padding
            u16  m_element_capacity;    // number of elements in the chunk
            u16  m_element_count;       // number of elements in the chunk
            u16  m_element_free_index;  // current element free index
            u16  m_pages_committed;     // number of committed pages in the chunk
            u64  m_bin0;                // binmap, level 0 (6)
            u64* m_bin1;                // binmap, level 1 (6) (fsa)
        };

        // A region of 4GiB consists of N blocks, maximum 65536 blocks.
        // The first block-size is 160 KiB; 4GiB / 160KiB = 26214 blocks, so 16 bit
        // should be sufficient to address all blocks in a region.
        struct block_t
        {
            u32 m_pages_committed;  // number of committed pages in the block
            u16 m_next;             // next/prev for linked list
            u16 m_prev;             // next/prev for linked list
        };

        struct bin_config_t
        {
            u32 m_alloc_size;        // size of elements in this bin
            u8  m_chunk_size_shift;  // chunk size shift for chunks in this bin
            u8  m_region_type;       // region type is chunk or block (0=chunks, 1=blocks)
            u8  m_bin_index;         // bin index
            u8  m_padding0;          // padding
        };

        typedef u8 (*size_to_bin_fn)(u32 size);

#define D_MAX_CHUNKS_PER_REGION 65536

        // A region consists of N chunks/blocks, maximum 65536 chunks/blocks.
        // All chunks in a region are of the same size (chunk_size_shift).
        struct region_t
        {
            struct chunks_t
            {
                chunk_t* m_free_list;  // list of free chunks in this region
                chunk_t* m_array;      // array of chunks
            };
            struct blocks_t
            {
                block_t* m_free_list;  // list of free blocks in this region
                block_t* m_array;      // array of blocks
            };

            u8  m_chunk_size_shift;        // chunk/block size shift for chunks/blocks in this region
            u8  m_region_type;             // region type is chunk or block (0=chunks, 1=blocks)
            u16 m_region_committed_pages;  // current number of committed pages for this region
            u16 m_region_maximum_pages;    // maximum number of committed pages for this region
            u32 m_free_index;              // index of the first free chunk/block in the region
            u32 m_free_index_threshold;    // threshold to trigger next page commit
            union
            {
                chunks_t m_chunks;
                blocks_t m_blocks;
            };
        };

        inline bool chunk_is_full(chunk_t* chunk) { return chunk->m_element_count == chunk->m_element_capacity; }
        inline bool chunk_is_empty(chunk_t* chunk) { return chunk->m_element_count == 0; }
        inline u32  region_chunk_index(region_t* region, chunk_t* chunk) { return (u32)(((byte*)chunk - (byte*)region->m_chunks.m_array) / sizeof(chunk_t)); }
        inline bool region_is_block_based(region_t* region) { return region->m_region_type == 1; }

        static void* alloc_from_chunk(chunk_t* chunk, byte* chunk_address, const bin_config_t& bin)
        {
            ASSERT(chunk->m_element_count < chunk->m_element_capacity);
            if (chunk->m_bin1 == nullptr)
            {
                const i32 free_index = nbinmap6::find_and_set(&chunk->m_bin0, chunk->m_element_count);
                if (free_index < 0)
                    return nullptr;
                chunk->m_element_count += 1;
                return chunk_address + (free_index * bin.m_alloc_size);
            }
            else
            {
                const i32 free_index = nbinmap12::find_and_set(&chunk->m_bin0, chunk->m_bin1, chunk->m_element_count);
                if (free_index < 0)
                    return nullptr;
                chunk->m_element_count += 1;
                return chunk_address + (free_index * bin.m_alloc_size);
            }
            return nullptr;
        }

#define D_CHUNK_CFG(chunk_size_shift) (((u8)((chunk_size_shift) - 10)) & 0x1F)
#define D_CHUNK_16KiB                 D_CHUNK_CFG(c16KiB)   //  16 KiB
#define D_CHUNK_32KiB                 D_CHUNK_CFG(c32KiB)   //  32 KiB
#define D_CHUNK_64KiB                 D_CHUNK_CFG(c64KiB)   //  64 KiB
#define D_CHUNK_128KiB                D_CHUNK_CFG(c128KiB)  //  128 KiB
#define D_CHUNK_256KiB                D_CHUNK_CFG(c256KiB)  //  256 KiB
#define D_CHUNK_512KiB                D_CHUNK_CFG(c512KiB)  //  512 KiB
#define D_CHUNK_1MiB                  D_CHUNK_CFG(c1MiB)    //    1 MiB
#define D_CHUNK_2MiB                  D_CHUNK_CFG(c2MiB)    //    2 MiB
#define D_CHUNK_4MiB                  D_CHUNK_CFG(c4MiB)    //    4 MiB
#define D_CHUNK_8MiB                  D_CHUNK_CFG(c8MiB)    //    8 MiB
#define D_CHUNK_16MiB                 D_CHUNK_CFG(c16MiB)   //   16 MiB
#define D_CHUNK_32MiB                 D_CHUNK_CFG(c32MiB)   //   32 MiB
#define D_CHUNK_64MiB                 D_CHUNK_CFG(c64MiB)   //   64 MiB
#define D_CHUNK_128MiB                D_CHUNK_CFG(c128MiB)  //  128 MiB
#define D_CHUNK_256MiB                D_CHUNK_CFG(c256MiB)  //  256 MiB
#define D_CHUNK_512MiB                D_CHUNK_CFG(c512MiB)  //  512 MiB

#define D_ALLOC_SIZE(mb, kb, b) (((((u32)(mb)) & 0xFFFF) << 20) | ((((u32)(kb)) & 0x3FF) << 10) | (((u32)(b)) & 0x3FF))

#define D_USAGE_CHUNKS 0
#define D_USAGE_BLOCKS 1

        static const bin_config_t sBinConfigs[] = {
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_USAGE_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 24), D_CHUNK_64KiB, D_USAGE_CHUNKS, 20},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 24), D_CHUNK_64KiB, D_USAGE_CHUNKS, 20},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 24), D_CHUNK_64KiB, D_USAGE_CHUNKS, 20},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 24), D_CHUNK_64KiB, D_USAGE_CHUNKS, 20},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 32), D_CHUNK_64KiB, D_USAGE_CHUNKS, 24},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 32), D_CHUNK_64KiB, D_USAGE_CHUNKS, 24},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 32), D_CHUNK_64KiB, D_USAGE_CHUNKS, 24},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 32), D_CHUNK_64KiB, D_USAGE_CHUNKS, 24},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 40), D_CHUNK_64KiB, D_USAGE_CHUNKS, 26},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 40), D_CHUNK_64KiB, D_USAGE_CHUNKS, 26},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 48), D_CHUNK_64KiB, D_USAGE_CHUNKS, 28},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 48), D_CHUNK_64KiB, D_USAGE_CHUNKS, 28},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 56), D_CHUNK_64KiB, D_USAGE_CHUNKS, 30},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 56), D_CHUNK_64KiB, D_USAGE_CHUNKS, 30},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 64), D_CHUNK_64KiB, D_USAGE_CHUNKS, 32},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 64), D_CHUNK_64KiB, D_USAGE_CHUNKS, 32},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 80), D_CHUNK_64KiB, D_USAGE_CHUNKS, 34},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 80), D_CHUNK_64KiB, D_USAGE_CHUNKS, 34},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 96), D_CHUNK_64KiB, D_USAGE_CHUNKS, 36},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 96), D_CHUNK_64KiB, D_USAGE_CHUNKS, 36},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 112), D_CHUNK_64KiB, D_USAGE_CHUNKS, 38},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 112), D_CHUNK_64KiB, D_USAGE_CHUNKS, 38},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 128), D_CHUNK_64KiB, D_USAGE_CHUNKS, 40},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 128), D_CHUNK_64KiB, D_USAGE_CHUNKS, 40},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 160), D_CHUNK_64KiB, D_USAGE_CHUNKS, 42},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 160), D_CHUNK_64KiB, D_USAGE_CHUNKS, 42},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 192), D_CHUNK_64KiB, D_USAGE_CHUNKS, 44},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 192), D_CHUNK_64KiB, D_USAGE_CHUNKS, 44},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 224), D_CHUNK_64KiB, D_USAGE_CHUNKS, 46},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 224), D_CHUNK_64KiB, D_USAGE_CHUNKS, 46},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 256), D_CHUNK_64KiB, D_USAGE_CHUNKS, 48},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 256), D_CHUNK_64KiB, D_USAGE_CHUNKS, 48},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 288), D_CHUNK_64KiB, D_USAGE_CHUNKS, 49},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 320), D_CHUNK_64KiB, D_USAGE_CHUNKS, 50},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 352), D_CHUNK_64KiB, D_USAGE_CHUNKS, 51},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 384), D_CHUNK_64KiB, D_USAGE_CHUNKS, 52},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 448), D_CHUNK_64KiB, D_USAGE_CHUNKS, 54},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 448), D_CHUNK_64KiB, D_USAGE_CHUNKS, 54},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 512), D_CHUNK_64KiB, D_USAGE_CHUNKS, 56},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 512), D_CHUNK_64KiB, D_USAGE_CHUNKS, 56},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 640), D_CHUNK_64KiB, D_USAGE_CHUNKS, 58},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 640), D_CHUNK_64KiB, D_USAGE_CHUNKS, 58},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 768), D_CHUNK_64KiB, D_USAGE_CHUNKS, 60},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 768), D_CHUNK_64KiB, D_USAGE_CHUNKS, 60},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 896), D_CHUNK_64KiB, D_USAGE_CHUNKS, 62},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 896), D_CHUNK_64KiB, D_USAGE_CHUNKS, 62},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 960), D_CHUNK_64KiB, D_USAGE_CHUNKS, 63},    //
          bin_config_t{D_ALLOC_SIZE(0, 1, 000), D_CHUNK_64KiB, D_USAGE_CHUNKS, 64},    //
          bin_config_t{D_ALLOC_SIZE(0, 1, 128), D_CHUNK_64KiB, D_USAGE_CHUNKS, 65},    //
          bin_config_t{D_ALLOC_SIZE(0, 1, 256), D_CHUNK_128KiB, D_USAGE_CHUNKS, 66},   //
          bin_config_t{D_ALLOC_SIZE(0, 1, 384), D_CHUNK_128KiB, D_USAGE_CHUNKS, 67},   //
          bin_config_t{D_ALLOC_SIZE(0, 1, 512), D_CHUNK_128KiB, D_USAGE_CHUNKS, 68},   //
          bin_config_t{D_ALLOC_SIZE(0, 1, 640), D_CHUNK_128KiB, D_USAGE_CHUNKS, 69},   //
          bin_config_t{D_ALLOC_SIZE(0, 1, 768), D_CHUNK_128KiB, D_USAGE_CHUNKS, 70},   //
          bin_config_t{D_ALLOC_SIZE(0, 1, 896), D_CHUNK_128KiB, D_USAGE_CHUNKS, 71},   //
          bin_config_t{D_ALLOC_SIZE(0, 2, 000), D_CHUNK_128KiB, D_USAGE_CHUNKS, 72},   //
          bin_config_t{D_ALLOC_SIZE(0, 2, 256), D_CHUNK_128KiB, D_USAGE_CHUNKS, 73},   //
          bin_config_t{D_ALLOC_SIZE(0, 2, 512), D_CHUNK_128KiB, D_USAGE_CHUNKS, 74},   //
          bin_config_t{D_ALLOC_SIZE(0, 2, 768), D_CHUNK_128KiB, D_USAGE_CHUNKS, 75},   //
          bin_config_t{D_ALLOC_SIZE(0, 3, 000), D_CHUNK_128KiB, D_USAGE_CHUNKS, 76},   //
          bin_config_t{D_ALLOC_SIZE(0, 3, 256), D_CHUNK_128KiB, D_USAGE_CHUNKS, 77},   //
          bin_config_t{D_ALLOC_SIZE(0, 3, 512), D_CHUNK_128KiB, D_USAGE_CHUNKS, 78},   //
          bin_config_t{D_ALLOC_SIZE(0, 3, 768), D_CHUNK_128KiB, D_USAGE_CHUNKS, 79},   //
          bin_config_t{D_ALLOC_SIZE(0, 4, 000), D_CHUNK_128KiB, D_USAGE_CHUNKS, 80},   //
          bin_config_t{D_ALLOC_SIZE(0, 4, 512), D_CHUNK_128KiB, D_USAGE_CHUNKS, 81},   //
          bin_config_t{D_ALLOC_SIZE(0, 5, 000), D_CHUNK_128KiB, D_USAGE_CHUNKS, 82},   //
          bin_config_t{D_ALLOC_SIZE(0, 5, 512), D_CHUNK_128KiB, D_USAGE_CHUNKS, 83},   //
          bin_config_t{D_ALLOC_SIZE(0, 6, 000), D_CHUNK_128KiB, D_USAGE_CHUNKS, 84},   //
          bin_config_t{D_ALLOC_SIZE(0, 6, 512), D_CHUNK_128KiB, D_USAGE_CHUNKS, 85},   //
          bin_config_t{D_ALLOC_SIZE(0, 7, 000), D_CHUNK_128KiB, D_USAGE_CHUNKS, 86},   //
          bin_config_t{D_ALLOC_SIZE(0, 7, 512), D_CHUNK_128KiB, D_USAGE_CHUNKS, 87},   //
          bin_config_t{D_ALLOC_SIZE(0, 8, 000), D_CHUNK_128KiB, D_USAGE_CHUNKS, 88},   //
          bin_config_t{D_ALLOC_SIZE(0, 9, 000), D_CHUNK_512KiB, D_USAGE_CHUNKS, 89},   //
          bin_config_t{D_ALLOC_SIZE(0, 10, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 90},    //
          bin_config_t{D_ALLOC_SIZE(0, 11, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 91},    //
          bin_config_t{D_ALLOC_SIZE(0, 12, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 92},    //
          bin_config_t{D_ALLOC_SIZE(0, 13, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 93},    //
          bin_config_t{D_ALLOC_SIZE(0, 14, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 94},    //
          bin_config_t{D_ALLOC_SIZE(0, 15, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 95},    //
          bin_config_t{D_ALLOC_SIZE(0, 16, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 96},    //
          bin_config_t{D_ALLOC_SIZE(0, 18, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 97},    //
          bin_config_t{D_ALLOC_SIZE(0, 20, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 98},    //
          bin_config_t{D_ALLOC_SIZE(0, 22, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 99},    //
          bin_config_t{D_ALLOC_SIZE(0, 24, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 100},   //
          bin_config_t{D_ALLOC_SIZE(0, 26, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 101},   //
          bin_config_t{D_ALLOC_SIZE(0, 28, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 102},   //
          bin_config_t{D_ALLOC_SIZE(0, 30, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 103},   //
          bin_config_t{D_ALLOC_SIZE(0, 32, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 104},   //
          bin_config_t{D_ALLOC_SIZE(0, 36, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 105},   //
          bin_config_t{D_ALLOC_SIZE(0, 40, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 106},   //
          bin_config_t{D_ALLOC_SIZE(0, 44, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 107},   //
          bin_config_t{D_ALLOC_SIZE(0, 48, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 108},   //
          bin_config_t{D_ALLOC_SIZE(0, 52, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 109},   //
          bin_config_t{D_ALLOC_SIZE(0, 56, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 110},   //
          bin_config_t{D_ALLOC_SIZE(0, 60, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 111},   //
          bin_config_t{D_ALLOC_SIZE(0, 64, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 112},   //
          bin_config_t{D_ALLOC_SIZE(0, 72, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 113},   //
          bin_config_t{D_ALLOC_SIZE(0, 80, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 114},   //
          bin_config_t{D_ALLOC_SIZE(0, 88, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 115},   //
          bin_config_t{D_ALLOC_SIZE(0, 96, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 116},   //
          bin_config_t{D_ALLOC_SIZE(0, 104, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 117},  //
          bin_config_t{D_ALLOC_SIZE(0, 112, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 118},  //
          bin_config_t{D_ALLOC_SIZE(0, 120, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 119},  //
          bin_config_t{D_ALLOC_SIZE(0, 128, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 120},  //
          bin_config_t{D_ALLOC_SIZE(0, 144, 0), D_CHUNK_512KiB, D_USAGE_CHUNKS, 121},  //
          bin_config_t{D_ALLOC_SIZE(0, 160, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 122},    //
          bin_config_t{D_ALLOC_SIZE(0, 176, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 123},    //
          bin_config_t{D_ALLOC_SIZE(0, 192, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 124},    //
          bin_config_t{D_ALLOC_SIZE(0, 208, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 125},    //
          bin_config_t{D_ALLOC_SIZE(0, 224, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 126},    //
          bin_config_t{D_ALLOC_SIZE(0, 240, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 127},    //
          bin_config_t{D_ALLOC_SIZE(0, 256, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 128},    //
          bin_config_t{D_ALLOC_SIZE(0, 288, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 129},    //
          bin_config_t{D_ALLOC_SIZE(0, 320, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 130},    //
          bin_config_t{D_ALLOC_SIZE(0, 352, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 131},    //
          bin_config_t{D_ALLOC_SIZE(0, 384, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 132},    //
          bin_config_t{D_ALLOC_SIZE(0, 416, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 133},    //
          bin_config_t{D_ALLOC_SIZE(0, 448, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 134},    //
          bin_config_t{D_ALLOC_SIZE(0, 480, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 135},    //
          bin_config_t{D_ALLOC_SIZE(0, 512, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 136},    //
          bin_config_t{D_ALLOC_SIZE(0, 576, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 137},    //
          bin_config_t{D_ALLOC_SIZE(0, 640, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 138},    //
          bin_config_t{D_ALLOC_SIZE(0, 704, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 139},    //
          bin_config_t{D_ALLOC_SIZE(0, 768, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 140},    //
          bin_config_t{D_ALLOC_SIZE(0, 832, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 141},    //
          bin_config_t{D_ALLOC_SIZE(0, 896, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 142},    //
          bin_config_t{D_ALLOC_SIZE(0, 960, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 143},    //
          bin_config_t{D_ALLOC_SIZE(1, 0, 0), D_CHUNK_1MiB, D_USAGE_BLOCKS, 144},      //
          bin_config_t{D_ALLOC_SIZE(1, 128, 0), D_CHUNK_2MiB, D_USAGE_BLOCKS, 145},    //
          bin_config_t{D_ALLOC_SIZE(1, 256, 0), D_CHUNK_2MiB, D_USAGE_BLOCKS, 146},    //
          bin_config_t{D_ALLOC_SIZE(1, 384, 0), D_CHUNK_2MiB, D_USAGE_BLOCKS, 147},    //
          bin_config_t{D_ALLOC_SIZE(1, 512, 0), D_CHUNK_2MiB, D_USAGE_BLOCKS, 148},    //
          bin_config_t{D_ALLOC_SIZE(1, 640, 0), D_CHUNK_2MiB, D_USAGE_BLOCKS, 149},    //
          bin_config_t{D_ALLOC_SIZE(1, 768, 0), D_CHUNK_2MiB, D_USAGE_BLOCKS, 150},    //
          bin_config_t{D_ALLOC_SIZE(1, 896, 0), D_CHUNK_2MiB, D_USAGE_BLOCKS, 151},    //
          bin_config_t{D_ALLOC_SIZE(2, 0, 0), D_CHUNK_2MiB, D_USAGE_BLOCKS, 152},      //
          bin_config_t{D_ALLOC_SIZE(2, 256, 0), D_CHUNK_4MiB, D_USAGE_BLOCKS, 153},    //
          bin_config_t{D_ALLOC_SIZE(2, 512, 0), D_CHUNK_4MiB, D_USAGE_BLOCKS, 154},    //
          bin_config_t{D_ALLOC_SIZE(2, 768, 0), D_CHUNK_4MiB, D_USAGE_BLOCKS, 155},    //
          bin_config_t{D_ALLOC_SIZE(3, 0, 0), D_CHUNK_4MiB, D_USAGE_BLOCKS, 156},      //
          bin_config_t{D_ALLOC_SIZE(3, 256, 0), D_CHUNK_4MiB, D_USAGE_BLOCKS, 157},    //
          bin_config_t{D_ALLOC_SIZE(3, 512, 0), D_CHUNK_4MiB, D_USAGE_BLOCKS, 158},    //
          bin_config_t{D_ALLOC_SIZE(3, 768, 0), D_CHUNK_4MiB, D_USAGE_BLOCKS, 159},    //
          bin_config_t{D_ALLOC_SIZE(4, 0, 0), D_CHUNK_4MiB, D_USAGE_BLOCKS, 160},      //
          bin_config_t{D_ALLOC_SIZE(4, 512, 0), D_CHUNK_8MiB, D_USAGE_BLOCKS, 161},    //
          bin_config_t{D_ALLOC_SIZE(5, 0, 0), D_CHUNK_8MiB, D_USAGE_BLOCKS, 162},      //
          bin_config_t{D_ALLOC_SIZE(5, 512, 0), D_CHUNK_8MiB, D_USAGE_BLOCKS, 163},    //
          bin_config_t{D_ALLOC_SIZE(6, 0, 0), D_CHUNK_8MiB, D_USAGE_BLOCKS, 164},      //
          bin_config_t{D_ALLOC_SIZE(6, 512, 0), D_CHUNK_8MiB, D_USAGE_BLOCKS, 165},    //
          bin_config_t{D_ALLOC_SIZE(7, 0, 0), D_CHUNK_8MiB, D_USAGE_BLOCKS, 166},      //
          bin_config_t{D_ALLOC_SIZE(7, 512, 0), D_CHUNK_8MiB, D_USAGE_BLOCKS, 167},    //
          bin_config_t{D_ALLOC_SIZE(8, 0, 0), D_CHUNK_8MiB, D_USAGE_BLOCKS, 168},      //
          bin_config_t{D_ALLOC_SIZE(9, 0, 0), D_CHUNK_16MiB, D_USAGE_BLOCKS, 169},     //
          bin_config_t{D_ALLOC_SIZE(10, 0, 0), D_CHUNK_16MiB, D_USAGE_BLOCKS, 170},    //
          bin_config_t{D_ALLOC_SIZE(11, 0, 0), D_CHUNK_16MiB, D_USAGE_BLOCKS, 171},    //
          bin_config_t{D_ALLOC_SIZE(12, 0, 0), D_CHUNK_16MiB, D_USAGE_BLOCKS, 172},    //
          bin_config_t{D_ALLOC_SIZE(13, 0, 0), D_CHUNK_16MiB, D_USAGE_BLOCKS, 173},    //
          bin_config_t{D_ALLOC_SIZE(14, 0, 0), D_CHUNK_16MiB, D_USAGE_BLOCKS, 174},    //
          bin_config_t{D_ALLOC_SIZE(15, 0, 0), D_CHUNK_16MiB, D_USAGE_BLOCKS, 175},    //
          bin_config_t{D_ALLOC_SIZE(16, 0, 0), D_CHUNK_16MiB, D_USAGE_BLOCKS, 176},    //
          bin_config_t{D_ALLOC_SIZE(18, 0, 0), D_CHUNK_32MiB, D_USAGE_BLOCKS, 177},    //
          bin_config_t{D_ALLOC_SIZE(20, 0, 0), D_CHUNK_32MiB, D_USAGE_BLOCKS, 178},    //
          bin_config_t{D_ALLOC_SIZE(22, 0, 0), D_CHUNK_32MiB, D_USAGE_BLOCKS, 179},    //
          bin_config_t{D_ALLOC_SIZE(24, 0, 0), D_CHUNK_32MiB, D_USAGE_BLOCKS, 180},    //
          bin_config_t{D_ALLOC_SIZE(26, 0, 0), D_CHUNK_32MiB, D_USAGE_BLOCKS, 181},    //
          bin_config_t{D_ALLOC_SIZE(28, 0, 0), D_CHUNK_32MiB, D_USAGE_BLOCKS, 182},    //
          bin_config_t{D_ALLOC_SIZE(30, 0, 0), D_CHUNK_32MiB, D_USAGE_BLOCKS, 183},    //
          bin_config_t{D_ALLOC_SIZE(32, 0, 0), D_CHUNK_32MiB, D_USAGE_BLOCKS, 184},    //
          bin_config_t{D_ALLOC_SIZE(36, 0, 0), D_CHUNK_64MiB, D_USAGE_BLOCKS, 185},    //
          bin_config_t{D_ALLOC_SIZE(40, 0, 0), D_CHUNK_64MiB, D_USAGE_BLOCKS, 186},    //
          bin_config_t{D_ALLOC_SIZE(44, 0, 0), D_CHUNK_64MiB, D_USAGE_BLOCKS, 187},    //
          bin_config_t{D_ALLOC_SIZE(48, 0, 0), D_CHUNK_64MiB, D_USAGE_BLOCKS, 188},    //
          bin_config_t{D_ALLOC_SIZE(52, 0, 0), D_CHUNK_64MiB, D_USAGE_BLOCKS, 189},    //
          bin_config_t{D_ALLOC_SIZE(56, 0, 0), D_CHUNK_64MiB, D_USAGE_BLOCKS, 190},    //
          bin_config_t{D_ALLOC_SIZE(60, 0, 0), D_CHUNK_64MiB, D_USAGE_BLOCKS, 191},    //
          bin_config_t{D_ALLOC_SIZE(64, 0, 0), D_CHUNK_64MiB, D_USAGE_BLOCKS, 192},    //
          bin_config_t{D_ALLOC_SIZE(72, 0, 0), D_CHUNK_128MiB, D_USAGE_BLOCKS, 193},   //
          bin_config_t{D_ALLOC_SIZE(80, 0, 0), D_CHUNK_128MiB, D_USAGE_BLOCKS, 194},   //
          bin_config_t{D_ALLOC_SIZE(88, 0, 0), D_CHUNK_128MiB, D_USAGE_BLOCKS, 195},   //
          bin_config_t{D_ALLOC_SIZE(96, 0, 0), D_CHUNK_128MiB, D_USAGE_BLOCKS, 196},   //
          bin_config_t{D_ALLOC_SIZE(104, 0, 0), D_CHUNK_128MiB, D_USAGE_BLOCKS, 197},  //
          bin_config_t{D_ALLOC_SIZE(112, 0, 0), D_CHUNK_128MiB, D_USAGE_BLOCKS, 198},  //
          bin_config_t{D_ALLOC_SIZE(120, 0, 0), D_CHUNK_128MiB, D_USAGE_BLOCKS, 199},  //
          bin_config_t{D_ALLOC_SIZE(128, 0, 0), D_CHUNK_128MiB, D_USAGE_BLOCKS, 200},  //
          bin_config_t{D_ALLOC_SIZE(144, 0, 0), D_CHUNK_256MiB, D_USAGE_BLOCKS, 201},  //
          bin_config_t{D_ALLOC_SIZE(160, 0, 0), D_CHUNK_256MiB, D_USAGE_BLOCKS, 202},  //
          bin_config_t{D_ALLOC_SIZE(176, 0, 0), D_CHUNK_256MiB, D_USAGE_BLOCKS, 203},  //
          bin_config_t{D_ALLOC_SIZE(192, 0, 0), D_CHUNK_256MiB, D_USAGE_BLOCKS, 204},  //
          bin_config_t{D_ALLOC_SIZE(208, 0, 0), D_CHUNK_256MiB, D_USAGE_BLOCKS, 205},  //
          bin_config_t{D_ALLOC_SIZE(224, 0, 0), D_CHUNK_256MiB, D_USAGE_BLOCKS, 206},  //
          bin_config_t{D_ALLOC_SIZE(240, 0, 0), D_CHUNK_256MiB, D_USAGE_BLOCKS, 207},  //
          bin_config_t{D_ALLOC_SIZE(256, 0, 0), D_CHUNK_256MiB, D_USAGE_BLOCKS, 208},  //
          bin_config_t{D_ALLOC_SIZE(288, 0, 0), D_CHUNK_512MiB, D_USAGE_BLOCKS, 209},  //
          bin_config_t{D_ALLOC_SIZE(320, 0, 0), D_CHUNK_512MiB, D_USAGE_BLOCKS, 210},  //
          bin_config_t{D_ALLOC_SIZE(352, 0, 0), D_CHUNK_512MiB, D_USAGE_BLOCKS, 211},  //
          bin_config_t{D_ALLOC_SIZE(384, 0, 0), D_CHUNK_512MiB, D_USAGE_BLOCKS, 212},  //
          bin_config_t{D_ALLOC_SIZE(416, 0, 0), D_CHUNK_512MiB, D_USAGE_BLOCKS, 213},  //
          bin_config_t{D_ALLOC_SIZE(448, 0, 0), D_CHUNK_512MiB, D_USAGE_BLOCKS, 214},  //
          bin_config_t{D_ALLOC_SIZE(480, 0, 0), D_CHUNK_512MiB, D_USAGE_BLOCKS, 215},  //
          bin_config_t{D_ALLOC_SIZE(512, 0, 0), D_CHUNK_512MiB, D_USAGE_BLOCKS, 216},  //
        };

        struct calloc_t
        {
            byte*          m_address_base;                  // base address of the superallocator
            int_t          m_address_size;                  // size of the address space
            arena_t*       m_internal_heap;                 // internal heap for initialization allocations
            u8             m_region_size_shift;             // size of each section (e.g. 4 GiB)
            u8             m_page_size_shift;               // system page size
            byte*          m_region_meta_base;              // base address of region metadata address space (N * 2MiB)
            u8             m_region_meta_size_shift;        // size of each region structure in 'regions_base' (e.g. 2 MiB)
            u32            m_region_meta_count;             // number of address sections in the whole address space
            u64            m_region_meta_free_bin0;         // binmap bin0 of free address sections
            u64*           m_region_meta_free_bin1;         // binmap bin1 of free address sections (heap)
            size_to_bin_fn m_size_to_bin;                   // function to map size to bin index
            bin_config_t*  m_bin_configs;                   // bin configurations
            region_t**     m_active_region_per_chunk_size;  // active region-lists per chunk config (heap)
            chunk_t**      m_active_chunk_per_bin_config;   // active chunk-lists per bin config (heap)
        };

        static inline region_t* get_region_at_index(calloc_t* c, u32 region_index) { return (region_t*)(c->m_region_meta_base + (region_index << c->m_region_meta_size_shift)); }
        static inline u32       get_region_index(calloc_t* c, region_t* region) { return (u32)(((byte*)region - c->m_region_meta_base) >> c->m_region_meta_size_shift); }
        static inline byte*     get_region_address(calloc_t* c, u32 region_index) { return c->m_address_base + (region_index << (c->m_region_size_shift + c->m_page_size_shift)); }
        static inline byte*     get_region_chunk_address(calloc_t* c, region_t* region, u32 chunk_index)
        {
            const u32 region_index   = get_region_index(c, region);
            byte*     region_address = get_region_address(c, region_index);
            return region_address + ((int_t)chunk_index << region->m_chunk_size_shift);
        }

        region_t* get_region(calloc_t* c, const bin_config_t& bin_config)
        {
            // see if we have an active region for this config
            // TODO this is actually a list (region should get next/prev pointers)
            region_t* active_region = c->m_active_region_per_chunk_size[bin_config.m_chunk_size_shift];
            if (active_region != nullptr)
                return active_region;

            // TODO obtain a new region and initialize it, this includes computing the amount of initial
            // pages that need to be committed.
            const i32 free_region = nbinmap12::find_and_set(&c->m_region_meta_free_bin0, c->m_region_meta_free_bin1, c->m_region_meta_count);
            if (free_region >= 0)
            {
                region_t* region         = get_region_at_index(c, (u32)free_region);
                byte*     region_address = get_region_address(c, (u32)free_region);

                // reset region
                region->m_chunk_size_shift       = bin_config.m_chunk_size_shift;
                region->m_region_type            = bin_config.m_region_type;
                region->m_region_committed_pages = 0;
                region->m_region_maximum_pages   = (1 << c->m_region_meta_size_shift) >> c->m_page_size_shift;
                region->m_free_index             = 0;
                region->m_free_index_threshold   = 0;
                region->m_chunks.m_array         = nullptr;
                region->m_chunks.m_free_list     = nullptr;

                // initialize region
                if (bin_config.m_region_type == D_USAGE_CHUNKS)
                {
                    ASSERT(region->m_capacity <= D_MAX_CHUNKS_PER_REGION);
                    v_alloc_commit(region_address, (int_t)1 << c->m_page_size_shift);  // commit first page
                    region->m_region_committed_pages = 1;
                    region->m_free_index_threshold   = (((int_t)1 << c->m_page_size_shift) - sizeof(region_t)) / sizeof(chunk_t);
                }
                c->m_active_region_per_chunk_size[bin_config.m_chunk_size_shift] = region;
                return region;
            }

            return nullptr;
        }

        void release_region(calloc_t* c, region_t* region)
        {
            ASSERT(region->m_count == 0);
            const u32 region_index = get_region_index(c, region);
            nbinmap12::clear(&c->m_region_meta_free_bin0, c->m_region_meta_free_bin1, region_index);
            v_alloc_decommit(get_region_address(c, region_index), region->m_region_committed_pages << c->m_page_size_shift);
        }

        chunk_t* get_chunk_from_region(calloc_t* c, region_t* region, const bin_config_t& bin_config)
        {
            if (region->m_chunks.m_free_list != nullptr)
            {
                chunk_t* chunk               = region->m_chunks.m_free_list;
                region->m_chunks.m_free_list = &region->m_chunks.m_array[chunk->m_next];
                chunk->m_prev                = nu16::NIL;
                chunk->m_next                = nu16::NIL;
                return chunk;
            }
            else if (region->m_free_index == region->m_free_index_threshold)
            {
                if (region->m_region_committed_pages == region->m_region_maximum_pages)
                    return nullptr;
                byte* region_address = get_region_address(c, get_region_index(c, region));
                v_alloc_commit(region_address + (region->m_region_committed_pages << c->m_page_size_shift), (int_t)1 << c->m_page_size_shift);
                region->m_region_committed_pages++;
                region->m_free_index_threshold = ((region->m_region_committed_pages << c->m_page_size_shift) - sizeof(region_t)) / sizeof(chunk_t);
            }
            chunk_t* chunk = &region->m_chunks.m_array[region->m_free_index];
            region->m_free_index++;
            return chunk;
        }

        void release_chunk_to_region(calloc_t* c, region_t* region, chunk_t* chunk)
        {
            chunk_t* head = region->m_chunks.m_free_list;
            if (head == nullptr)
            {
                chunk->m_prev = nu16::NIL;
                chunk->m_next = nu16::NIL;
            }
            else
            {
                chunk->m_prev = nu16::NIL;
                chunk->m_next = (u16)(region_chunk_index(region, head));
                head->m_prev  = (u16)(region_chunk_index(region, chunk));
            }
            region->m_chunks.m_free_list = chunk;
        }

        chunk_t* get_active_chunk(calloc_t* c, const bin_config_t& bin_config)
        {
            chunk_t* active_chunk = c->m_active_chunk_per_bin_config[bin_config.m_bin_index];
            return active_chunk;
        }

        void add_chunk_to_active_list(calloc_t* c, const bin_config_t& bin_config, chunk_t* chunk)
        {
            ASSERT(is_nil(chunk->m_prev) && is_nil(chunk->m_next));
            if (c->m_active_chunk_per_bin_config[bin_config.m_bin_index] == nullptr)
            {
                c->m_active_chunk_per_bin_config[bin_config.m_bin_index] = chunk;
            }
            else
            {
                chunk_t* head                                            = c->m_active_chunk_per_bin_config[bin_config.m_bin_index];
                chunk->m_next                                            = (u16)(region_chunk_index(nullptr, head));  // region is not needed here
                head->m_prev                                             = (u16)(region_chunk_index(nullptr, chunk));
                c->m_active_chunk_per_bin_config[bin_config.m_bin_index] = chunk;
            }
        }

        void remove_chunk_from_active_list(calloc_t* c, const bin_config_t& bin_config, chunk_t* chunk)
        {
            chunk_t*& head = c->m_active_chunk_per_bin_config[bin_config.m_bin_index];
            if (is_nil(chunk->m_prev) && is_nil(chunk->m_next))
            {
                head = nullptr;
            }
            else if (is_nil(chunk->m_prev))
            {
                region_t* region = get_region_at_index(c, chunk->m_region_index);
                chunk_t*  next   = &region->m_chunks.m_array[chunk->m_next];
                next->m_prev     = nu16::NIL;
                head             = next;
            }
            else if (is_nil(chunk->m_next))
            {
                region_t* region = get_region_at_index(c, chunk->m_region_index);
                chunk_t*  prev   = &region->m_chunks.m_array[chunk->m_prev];
                prev->m_next     = nu16::NIL;
            }
            else
            {
                region_t* region = get_region_at_index(c, chunk->m_region_index);
                chunk_t*  prev   = &region->m_chunks.m_array[chunk->m_prev];
                chunk_t*  next   = &region->m_chunks.m_array[chunk->m_next];
                prev->m_next     = chunk->m_next;
                next->m_prev     = chunk->m_prev;
            }
            chunk->m_prev = nu16::NIL;
            chunk->m_next = nu16::NIL;
        }

        // Allocate memory of given size
        void* alloc(calloc_t* c, u32 size)
        {
            const u8            bin_index  = c->m_size_to_bin(size);
            const bin_config_t& bin_config = c->m_bin_configs[bin_index];
            region_t*           region     = get_region(c, bin_config);
            if (region == nullptr)
                return nullptr;

            if (region_is_block_based(region))
            {
                // TODO, allocate a block from a region and make sure enough pages are committed
                return nullptr;
            }

            // allocate from chunk-based region
            chunk_t* chunk = get_active_chunk(c, bin_config);
            if (chunk == nullptr)
            {
                chunk = get_chunk_from_region(c, region, bin_config);
                if (chunk == nullptr)
                    return nullptr;
                add_chunk_to_active_list(c, bin_config, chunk);
            }

            byte* chunk_address = get_region_chunk_address(c, region, region_chunk_index(region, chunk));
            void* ptr           = alloc_from_chunk(chunk, chunk_address, bin_config);
            if (chunk_is_full(chunk))
            {
                remove_chunk_from_active_list(c, bin_config, chunk);
            }
            return ptr;
        }

        void dealloc(calloc_t* c, void* ptr)
        {
            const u32 region_index = (u32)(((const byte*)ptr - c->m_address_base) >> c->m_region_size_shift);
            ASSERTS(region_index >= 0 && region_entry < segment->m_region_count, "invalid region index");
            region_t*   region         = (region_t*)(c->m_region_meta_base + ((int_t)region_index << c->m_region_meta_size_shift));
            const byte* region_address = c->m_address_base + (region_index << c->m_region_size_shift);
            if (region_is_block_based(region))
            {
                const u32 block_index = ((const byte*)ptr - region_address) >> region->m_chunk_size_shift;
                // TODO, deallocating from a block:
                // - validate address within region
                // - calculate block index
                // - mark block as free
                // - if required, decommit pages
            }
            else
            {
                const u32   chunk_index       = ((const byte*)ptr - region_address) >> region->m_chunk_size_shift;
                chunk_t*    chunk             = &region->m_chunks.m_array[chunk_index];
                const byte* chunk_address     = region_address + (chunk_index << region->m_chunk_size_shift);
                const bool  chunk_full_before = chunk_is_full(chunk);
                if (chunk->m_bin1 == nullptr)
                {
                    nbinmap6::clr(&chunk->m_bin0, chunk->m_element_free_index, chunk->m_element_count);
                }
                else
                {
                    nbinmap12::clr(&chunk->m_bin0, chunk->m_bin1, chunk->m_element_free_index, chunk->m_element_count);
                }
                chunk->m_element_count -= 1;

                if (chunk_is_empty(chunk))
                {
                    // TODO:
                    // - remove chunk from active list
                    // - remove chunk from region
                    // - if region is now completely free:
                    //   - remove from segment
                    //   - if segment was full before, add to active segment list
                    //   - or if segment is now completely empty, release segment
                    // - else
                    //   - add chunk to free list ?
                }
                else if (chunk_full_before)
                {
                    // TODO add chunk to active list
                }
            }
        }

    }  // namespace nsuperallocv2

}  // namespace ncore
