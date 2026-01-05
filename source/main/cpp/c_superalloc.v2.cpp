#include "ccore/c_target.h"
#include "ccore/c_allocator.h"
#include "ccore/c_binmap1.h"
#include "ccore/c_debug.h"
#include "ccore/c_limits.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"
#include "ccore/c_arena.h"

#include "csuperalloc/private/c_doubly_linked_list.h"
#include "csuperalloc/c_superalloc.h"
#include "csuperalloc/c_superalloc_config.h"

namespace ncore
{
    namespace nsuperallocv2
    {
        // A chunk consists of N elements with a maximum of 4K
        struct chunk_t
        {
            i8   m_region_index;        // index into region array (back reference)
            i8   m_region_chunk_index;  // index of the chunk in the region (back reference)
            i8   m_shift_size;          // shift size of the chunk (1 << shift_size = chunk size)
            i8   m_bin_levels;          // number of bin levels in the binmap for free elements
            u32  m_pages_committed;     // number of committed pages in the chunk
            u32  m_element_size;        // size of each element in the chunk
            u32  m_element_capacity;    // number of elements in the chunk
            u32  m_element_count;       // number of elements in the chunk
            u32  m_element_free_index;  // current element free index
            u64  m_bin0;                // binmap, level 0 (6)
            u64* m_bin1;                // binmap, level 1 (6)
            u32  m_chunk_prev;          // previous chunk in the list
            u32  m_chunk_next;          // next chunk in the list
        };

        struct config_t
        {
            inline config_t(u16 d)
                : m_data(d)
            {
            }
            inline i8 get_index() const { return (i8)((m_data >> 0) & 0x1F); }                     // config index
            inline i8 get_section_size_shift() const { return 32; }                                // segment size shift is always 32 (4 GiB)
            inline i8 get_segment_size_shift() const { return (i8)((m_data >> 15) & 0x1F) + 10; }  // segment size shift
            inline i8 get_region_size_shift() const { return (i8)((m_data >> 5) & 0x1F) + 10; }    // region size shift
            inline i8 get_chunk_size_shift() const { return (i8)((m_data >> 0) & 0x1F) + 10; }     // chunk / block size shift
            u16       m_data;
        };

        struct bin_config_t
        {
            u32      m_alloc_size;    // size of elements in this bin
            config_t m_config;        // config for this bin
            u8       m_region_usage;  // region is chunk or block based (0=chunks, 1=blocks)
            u8       m_bin_index;     // bin index
        };

        typedef u8 (*size_to_bin_fn)(u32 size);

        // A region consists of N chunks/blocks with a maximum of 64
        struct region_t
        {
            u64  m_free_map;       // bit map of free chunks / blocks in this region
            u32* m_array;          // array of chunk indices or pages per block
            u8   m_region_usage;   // region is chunk or block based (0=chunks, 1=blocks)
            u16  m_segment_index;  // index of segment that we are part of (back reference)
            u16  m_region_index;   // index of region in the region array of segment
            u32  m_prev;           // previous region in the list
            u32  m_next;           // next region in the list
        };

        // A segment consists of N regions with a maximum of 64
        struct segment_t
        {
            config_t m_config;                 // region configuration for this segment
            u16      m_segment_address_index;  // address index of this segment
            u16      m_region_count;           // current number of regions for this segment
            u16      m_region_capacity;        // maximum number of regions for this segment
            u64      m_region_free_bin0;       // binmap bin0 of free regions for this segment
            u32*     m_region_array;           // array of regions for this segment
            u32      m_prev;                   // previous segment in the list
            u32      m_next;                   // next segment in the list
        };

        // A section consists of N segments/regions with a maximum of 64, or for large allocation
        // we are directly
        struct section_t
        {
            u8   m_capacity;  // number of segments in this section
            u8   m_count;     // number of segments used in this section
            u8   m_usage;     // usage type, 0 = segments, 1 = regions
            u8   m_padding;   // padding
            u64  m_free_map;  // binmap bin0 of free segments/regions for this section
            u32* m_array;     // array of segments/regions/blocks for this section
        };

        inline bool chunk_is_full(chunk_t* chunk) { return chunk->m_element_count == chunk->m_element_capacity; }
        inline bool chunk_is_empty(chunk_t* chunk) { return chunk->m_element_count == 0; }

        static void* alloc_from_chunk(chunk_t* chunk, byte* chunk_address)
        {
            ASSERT(chunk->m_element_count < chunk->m_element_capacity);
            switch (chunk->m_bin_levels)
            {
                case 0:
                {
                    const i32 free_index = nbinmap6::find_and_set(&chunk->m_bin0, chunk->m_element_count);
                    if (free_index < 0)
                        return nullptr;
                    chunk->m_element_count += 1;
                    return chunk_address + (free_index * chunk->m_element_size);
                }
                case 1:
                {
                    const i32 free_index = nbinmap12::find_and_set(&chunk->m_bin0, chunk->m_bin1, chunk->m_element_count);
                    if (free_index < 0)
                        return nullptr;
                    chunk->m_element_count += 1;
                    return chunk_address + (free_index * chunk->m_element_size);
                }
            }

            return nullptr;
        }

        // We introduce an intermediate structure called region to reduce the impact of small chunks to the higher level.
        // A region has a variable size (4 MiB to 1 GiB) and consists of N same size chunks, the following types are defined:
        //   - 4 MiB, 64 chunks of 64 KiB
        //   - 8 MiB, 64 chunks of 128 KiB
        //   - 16 MiB, 64 chunks of 256 KiB
        //   - 32 MiB, 64 chunks of 512 KiB
        //   - 64 MiB, 64 chunks of 1 MiB
        //   - 128 MiB, 64 chunks of 2 MiB
        //   - 256 MiB, 64 chunks of 4 MiB
        //   - 512 MiB, 64 chunks of 8 MiB
        //   - 1 GiB, 64 chunks of 16 MiB

        // A region can also be configured to handle large allocations (m_region_type = c_region_type_blocks).
        // Allocation sizes handled are for example:
        //   region size,        block size,                  allocation sizes
        //     16 MiB,             256 KiB,                 128 KiB upto 256 KiB
        //     32 MiB,             512 KiB,                 256 KiB upto 512 KiB
        //     64 MiB,             1 MiB,                   512 KiB upto 1 MiB
        //    128 MiB,             2 MiB,                   1 MiB upto 2 MiB
        //    256 MiB,             4 MiB,                   2 MiB upto 4 MiB
        //    512 MiB,             8 MiB,                   4 MiB upto 8 MiB
        //      1 GiB,            16 MiB / 512 MiB,         8 MiB upto 16 MiB / 512 MiB

        enum sizeshift_e
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

#define D_CHUNK_CFG(segment_size_shift, region_size_shift, chunk_size_shift) (((((u16)((segment_size_shift) - 10)) & 0x1F) << 15) | ((((u16)((region_size_shift) - 10)) & 0x1F) << 5) | (((u16)((chunk_size_shift) - 10)) & 0x1F))

//                                                              section   |   segment   |     region   |  chunk/block
#define D_CHUNK_16KiB  D_CHUNK_CFG(c64MiB, c1MiB, c16KiB)    // 4 GiB/64 ->  64 MiB/64 ->    1 MiB/64 -> 16 KiB
#define D_CHUNK_32KiB  D_CHUNK_CFG(c128MiB, c2MiB, c32KiB)   // 4 GiB/32 -> 128 MiB/64 ->    2 MiB/64 -> 32 KiB
#define D_CHUNK_64KiB  D_CHUNK_CFG(c256MiB, c4MiB, c64KiB)   // 4 GiB/16 -> 256 MiB/64 ->    4 MiB/64 -> 64 KiB
#define D_CHUNK_128KiB D_CHUNK_CFG(c512MiB, c8MiB, c128KiB)  // 4 GiB/8  -> 512 MiB/64 ->    8 MiB/64 -> 128 KiB
#define D_CHUNK_256KiB D_CHUNK_CFG(c1GiB, c16MiB, c256KiB)   // 4 GiB/4  ->   1 GiB/64 ->   16 MiB/64 -> 256 KiB
#define D_CHUNK_512KiB D_CHUNK_CFG(c1GiB, c16MiB, c512KiB)   // 4 GiB/4  ->   1 GiB/64 ->   16 MiB/32 -> 512 KiB
#define D_CHUNK_1MiB   D_CHUNK_CFG(c1GiB, c16MiB, c1MiB)     // 4 GiB/4  ->   1 GiB/64 ->   16 MiB/16 ->   1 MiB
#define D_CHUNK_2MiB   D_CHUNK_CFG(c1GiB, c16MiB, c2MiB)     // 4 GiB/4  ->   1 GiB/64 ->   16 MiB/8  ->   2 MiB
#define D_CHUNK_4MiB   D_CHUNK_CFG(c1GiB, c16MiB, c4MiB)     // 4 GiB/4  ->   1 GiB/64 ->   16 MiB/4  ->   4 MiB
#define D_CHUNK_8MiB   D_CHUNK_CFG(c1GiB, c16MiB, c8MiB)     // 4 GiB/4  ->   1 GiB/64 ->   16 MiB/2  ->   8 MiB
#define D_CHUNK_16MiB  D_CHUNK_CFG(c1GiB, c16MiB, c16MiB)    // 4 GiB/4  ->                  1 GiB/64 ->  16 MiB
#define D_CHUNK_32MiB  D_CHUNK_CFG(c1GiB, c32MiB, c32MiB)    // 4 GiB/4  ->                  1 GiB/32 ->  32 MiB
#define D_CHUNK_64MiB  D_CHUNK_CFG(c1GiB, c64MiB, c64MiB)    // 4 GiB/4  ->                  1 GiB/16 ->  64 MiB
#define D_CHUNK_128MiB D_CHUNK_CFG(c1GiB, c128MiB, c128MiB)  // 4 GiB/4  ->                  1 GiB/8  -> 128 MiB
#define D_CHUNK_256MiB D_CHUNK_CFG(c1GiB, c256MiB, c256MiB)  // 4 GiB/4  ->                  1 GiB/4  -> 256 MiB
#define D_CHUNK_512MiB D_CHUNK_CFG(c1GiB, c512MiB, c512MiB)  // 4 GiB/4  ->                  1 GiB/2  -> 512 MiB

#define D_ALLOC_SIZE(mb, kb, b) (((((u32)(mb)) & 0xFFFF) << 20) | ((((u32)(kb)) & 0x3FF) << 10) | (((u32)(b)) & 0x3FF))
#define D_REGION_USING_CHUNKS   0
#define D_REGION_USING_BLOCKS   1

        static const bin_config_t sBinConfigs[] = {
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 16), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 16},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 24), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 20},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 24), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 20},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 24), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 20},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 24), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 20},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 32), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 24},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 32), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 24},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 32), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 24},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 32), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 24},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 40), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 26},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 40), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 26},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 48), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 28},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 48), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 28},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 56), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 30},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 56), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 30},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 64), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 32},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 64), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 32},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 80), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 34},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 80), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 34},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 96), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 36},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 96), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 36},     //
          bin_config_t{D_ALLOC_SIZE(0, 0, 112), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 38},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 112), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 38},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 128), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 40},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 128), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 40},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 160), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 42},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 160), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 42},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 192), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 44},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 192), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 44},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 224), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 46},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 224), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 46},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 256), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 48},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 256), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 48},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 288), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 49},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 320), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 50},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 352), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 51},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 384), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 52},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 448), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 54},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 448), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 54},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 512), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 56},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 512), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 56},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 640), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 58},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 640), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 58},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 768), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 60},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 768), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 60},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 896), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 62},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 896), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 62},    //
          bin_config_t{D_ALLOC_SIZE(0, 0, 960), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 63},    //
          bin_config_t{D_ALLOC_SIZE(0, 1, 000), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 64},    //
          bin_config_t{D_ALLOC_SIZE(0, 1, 128), D_CHUNK_64KiB, D_REGION_USING_CHUNKS, 65},    //
          bin_config_t{D_ALLOC_SIZE(0, 1, 256), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 66},   //
          bin_config_t{D_ALLOC_SIZE(0, 1, 384), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 67},   //
          bin_config_t{D_ALLOC_SIZE(0, 1, 512), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 68},   //
          bin_config_t{D_ALLOC_SIZE(0, 1, 640), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 69},   //
          bin_config_t{D_ALLOC_SIZE(0, 1, 768), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 70},   //
          bin_config_t{D_ALLOC_SIZE(0, 1, 896), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 71},   //
          bin_config_t{D_ALLOC_SIZE(0, 2, 000), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 72},   //
          bin_config_t{D_ALLOC_SIZE(0, 2, 256), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 73},   //
          bin_config_t{D_ALLOC_SIZE(0, 2, 512), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 74},   //
          bin_config_t{D_ALLOC_SIZE(0, 2, 768), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 75},   //
          bin_config_t{D_ALLOC_SIZE(0, 3, 000), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 76},   //
          bin_config_t{D_ALLOC_SIZE(0, 3, 256), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 77},   //
          bin_config_t{D_ALLOC_SIZE(0, 3, 512), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 78},   //
          bin_config_t{D_ALLOC_SIZE(0, 3, 768), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 79},   //
          bin_config_t{D_ALLOC_SIZE(0, 4, 000), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 80},   //
          bin_config_t{D_ALLOC_SIZE(0, 4, 512), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 81},   //
          bin_config_t{D_ALLOC_SIZE(0, 5, 000), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 82},   //
          bin_config_t{D_ALLOC_SIZE(0, 5, 512), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 83},   //
          bin_config_t{D_ALLOC_SIZE(0, 6, 000), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 84},   //
          bin_config_t{D_ALLOC_SIZE(0, 6, 512), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 85},   //
          bin_config_t{D_ALLOC_SIZE(0, 7, 000), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 86},   //
          bin_config_t{D_ALLOC_SIZE(0, 7, 512), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 87},   //
          bin_config_t{D_ALLOC_SIZE(0, 8, 000), D_CHUNK_128KiB, D_REGION_USING_CHUNKS, 88},   //
          bin_config_t{D_ALLOC_SIZE(0, 9, 000), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 89},   //
          bin_config_t{D_ALLOC_SIZE(0, 10, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 90},    //
          bin_config_t{D_ALLOC_SIZE(0, 11, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 91},    //
          bin_config_t{D_ALLOC_SIZE(0, 12, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 92},    //
          bin_config_t{D_ALLOC_SIZE(0, 13, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 93},    //
          bin_config_t{D_ALLOC_SIZE(0, 14, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 94},    //
          bin_config_t{D_ALLOC_SIZE(0, 15, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 95},    //
          bin_config_t{D_ALLOC_SIZE(0, 16, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 96},    //
          bin_config_t{D_ALLOC_SIZE(0, 18, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 97},    //
          bin_config_t{D_ALLOC_SIZE(0, 20, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 98},    //
          bin_config_t{D_ALLOC_SIZE(0, 22, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 99},    //
          bin_config_t{D_ALLOC_SIZE(0, 24, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 100},   //
          bin_config_t{D_ALLOC_SIZE(0, 26, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 101},   //
          bin_config_t{D_ALLOC_SIZE(0, 28, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 102},   //
          bin_config_t{D_ALLOC_SIZE(0, 30, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 103},   //
          bin_config_t{D_ALLOC_SIZE(0, 32, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 104},   //
          bin_config_t{D_ALLOC_SIZE(0, 36, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 105},   //
          bin_config_t{D_ALLOC_SIZE(0, 40, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 106},   //
          bin_config_t{D_ALLOC_SIZE(0, 44, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 107},   //
          bin_config_t{D_ALLOC_SIZE(0, 48, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 108},   //
          bin_config_t{D_ALLOC_SIZE(0, 52, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 109},   //
          bin_config_t{D_ALLOC_SIZE(0, 56, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 110},   //
          bin_config_t{D_ALLOC_SIZE(0, 60, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 111},   //
          bin_config_t{D_ALLOC_SIZE(0, 64, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 112},   //
          bin_config_t{D_ALLOC_SIZE(0, 72, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 113},   //
          bin_config_t{D_ALLOC_SIZE(0, 80, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 114},   //
          bin_config_t{D_ALLOC_SIZE(0, 88, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 115},   //
          bin_config_t{D_ALLOC_SIZE(0, 96, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 116},   //
          bin_config_t{D_ALLOC_SIZE(0, 104, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 117},  //
          bin_config_t{D_ALLOC_SIZE(0, 112, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 118},  //
          bin_config_t{D_ALLOC_SIZE(0, 120, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 119},  //
          bin_config_t{D_ALLOC_SIZE(0, 128, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 120},  //
          bin_config_t{D_ALLOC_SIZE(0, 144, 0), D_CHUNK_512KiB, D_REGION_USING_CHUNKS, 121},  //
          bin_config_t{D_ALLOC_SIZE(0, 160, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 122},    //
          bin_config_t{D_ALLOC_SIZE(0, 176, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 123},    //
          bin_config_t{D_ALLOC_SIZE(0, 192, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 124},    //
          bin_config_t{D_ALLOC_SIZE(0, 208, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 125},    //
          bin_config_t{D_ALLOC_SIZE(0, 224, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 126},    //
          bin_config_t{D_ALLOC_SIZE(0, 240, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 127},    //
          bin_config_t{D_ALLOC_SIZE(0, 256, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 128},    //
          bin_config_t{D_ALLOC_SIZE(0, 288, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 129},    //
          bin_config_t{D_ALLOC_SIZE(0, 320, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 130},    //
          bin_config_t{D_ALLOC_SIZE(0, 352, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 131},    //
          bin_config_t{D_ALLOC_SIZE(0, 384, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 132},    //
          bin_config_t{D_ALLOC_SIZE(0, 416, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 133},    //
          bin_config_t{D_ALLOC_SIZE(0, 448, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 134},    //
          bin_config_t{D_ALLOC_SIZE(0, 480, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 135},    //
          bin_config_t{D_ALLOC_SIZE(0, 512, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 136},    //
          bin_config_t{D_ALLOC_SIZE(0, 576, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 137},    //
          bin_config_t{D_ALLOC_SIZE(0, 640, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 138},    //
          bin_config_t{D_ALLOC_SIZE(0, 704, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 139},    //
          bin_config_t{D_ALLOC_SIZE(0, 768, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 140},    //
          bin_config_t{D_ALLOC_SIZE(0, 832, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 141},    //
          bin_config_t{D_ALLOC_SIZE(0, 896, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 142},    //
          bin_config_t{D_ALLOC_SIZE(0, 960, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 143},    //
          bin_config_t{D_ALLOC_SIZE(1, 0, 0), D_CHUNK_1MiB, D_REGION_USING_BLOCKS, 144},      //
          bin_config_t{D_ALLOC_SIZE(1, 128, 0), D_CHUNK_2MiB, D_REGION_USING_BLOCKS, 145},    //
          bin_config_t{D_ALLOC_SIZE(1, 256, 0), D_CHUNK_2MiB, D_REGION_USING_BLOCKS, 146},    //
          bin_config_t{D_ALLOC_SIZE(1, 384, 0), D_CHUNK_2MiB, D_REGION_USING_BLOCKS, 147},    //
          bin_config_t{D_ALLOC_SIZE(1, 512, 0), D_CHUNK_2MiB, D_REGION_USING_BLOCKS, 148},    //
          bin_config_t{D_ALLOC_SIZE(1, 640, 0), D_CHUNK_2MiB, D_REGION_USING_BLOCKS, 149},    //
          bin_config_t{D_ALLOC_SIZE(1, 768, 0), D_CHUNK_2MiB, D_REGION_USING_BLOCKS, 150},    //
          bin_config_t{D_ALLOC_SIZE(1, 896, 0), D_CHUNK_2MiB, D_REGION_USING_BLOCKS, 151},    //
          bin_config_t{D_ALLOC_SIZE(2, 0, 0), D_CHUNK_2MiB, D_REGION_USING_BLOCKS, 152},      //
          bin_config_t{D_ALLOC_SIZE(2, 256, 0), D_CHUNK_4MiB, D_REGION_USING_BLOCKS, 153},    //
          bin_config_t{D_ALLOC_SIZE(2, 512, 0), D_CHUNK_4MiB, D_REGION_USING_BLOCKS, 154},    //
          bin_config_t{D_ALLOC_SIZE(2, 768, 0), D_CHUNK_4MiB, D_REGION_USING_BLOCKS, 155},    //
          bin_config_t{D_ALLOC_SIZE(3, 0, 0), D_CHUNK_4MiB, D_REGION_USING_BLOCKS, 156},      //
          bin_config_t{D_ALLOC_SIZE(3, 256, 0), D_CHUNK_4MiB, D_REGION_USING_BLOCKS, 157},    //
          bin_config_t{D_ALLOC_SIZE(3, 512, 0), D_CHUNK_4MiB, D_REGION_USING_BLOCKS, 158},    //
          bin_config_t{D_ALLOC_SIZE(3, 768, 0), D_CHUNK_4MiB, D_REGION_USING_BLOCKS, 159},    //
          bin_config_t{D_ALLOC_SIZE(4, 0, 0), D_CHUNK_4MiB, D_REGION_USING_BLOCKS, 160},      //
          bin_config_t{D_ALLOC_SIZE(4, 512, 0), D_CHUNK_8MiB, D_REGION_USING_BLOCKS, 161},    //
          bin_config_t{D_ALLOC_SIZE(5, 0, 0), D_CHUNK_8MiB, D_REGION_USING_BLOCKS, 162},      //
          bin_config_t{D_ALLOC_SIZE(5, 512, 0), D_CHUNK_8MiB, D_REGION_USING_BLOCKS, 163},    //
          bin_config_t{D_ALLOC_SIZE(6, 0, 0), D_CHUNK_8MiB, D_REGION_USING_BLOCKS, 164},      //
          bin_config_t{D_ALLOC_SIZE(6, 512, 0), D_CHUNK_8MiB, D_REGION_USING_BLOCKS, 165},    //
          bin_config_t{D_ALLOC_SIZE(7, 0, 0), D_CHUNK_8MiB, D_REGION_USING_BLOCKS, 166},      //
          bin_config_t{D_ALLOC_SIZE(7, 512, 0), D_CHUNK_8MiB, D_REGION_USING_BLOCKS, 167},    //
          bin_config_t{D_ALLOC_SIZE(8, 0, 0), D_CHUNK_8MiB, D_REGION_USING_BLOCKS, 168},      //
          bin_config_t{D_ALLOC_SIZE(9, 0, 0), D_CHUNK_16MiB, D_REGION_USING_BLOCKS, 169},     //
          bin_config_t{D_ALLOC_SIZE(10, 0, 0), D_CHUNK_16MiB, D_REGION_USING_BLOCKS, 170},    //
          bin_config_t{D_ALLOC_SIZE(11, 0, 0), D_CHUNK_16MiB, D_REGION_USING_BLOCKS, 171},    //
          bin_config_t{D_ALLOC_SIZE(12, 0, 0), D_CHUNK_16MiB, D_REGION_USING_BLOCKS, 172},    //
          bin_config_t{D_ALLOC_SIZE(13, 0, 0), D_CHUNK_16MiB, D_REGION_USING_BLOCKS, 173},    //
          bin_config_t{D_ALLOC_SIZE(14, 0, 0), D_CHUNK_16MiB, D_REGION_USING_BLOCKS, 174},    //
          bin_config_t{D_ALLOC_SIZE(15, 0, 0), D_CHUNK_16MiB, D_REGION_USING_BLOCKS, 175},    //
          bin_config_t{D_ALLOC_SIZE(16, 0, 0), D_CHUNK_16MiB, D_REGION_USING_BLOCKS, 176},    //
          bin_config_t{D_ALLOC_SIZE(18, 0, 0), D_CHUNK_32MiB, D_REGION_USING_BLOCKS, 177},    //
          bin_config_t{D_ALLOC_SIZE(20, 0, 0), D_CHUNK_32MiB, D_REGION_USING_BLOCKS, 178},    //
          bin_config_t{D_ALLOC_SIZE(22, 0, 0), D_CHUNK_32MiB, D_REGION_USING_BLOCKS, 179},    //
          bin_config_t{D_ALLOC_SIZE(24, 0, 0), D_CHUNK_32MiB, D_REGION_USING_BLOCKS, 180},    //
          bin_config_t{D_ALLOC_SIZE(26, 0, 0), D_CHUNK_32MiB, D_REGION_USING_BLOCKS, 181},    //
          bin_config_t{D_ALLOC_SIZE(28, 0, 0), D_CHUNK_32MiB, D_REGION_USING_BLOCKS, 182},    //
          bin_config_t{D_ALLOC_SIZE(30, 0, 0), D_CHUNK_32MiB, D_REGION_USING_BLOCKS, 183},    //
          bin_config_t{D_ALLOC_SIZE(32, 0, 0), D_CHUNK_32MiB, D_REGION_USING_BLOCKS, 184},    //
          bin_config_t{D_ALLOC_SIZE(36, 0, 0), D_CHUNK_64MiB, D_REGION_USING_BLOCKS, 185},    //
          bin_config_t{D_ALLOC_SIZE(40, 0, 0), D_CHUNK_64MiB, D_REGION_USING_BLOCKS, 186},    //
          bin_config_t{D_ALLOC_SIZE(44, 0, 0), D_CHUNK_64MiB, D_REGION_USING_BLOCKS, 187},    //
          bin_config_t{D_ALLOC_SIZE(48, 0, 0), D_CHUNK_64MiB, D_REGION_USING_BLOCKS, 188},    //
          bin_config_t{D_ALLOC_SIZE(52, 0, 0), D_CHUNK_64MiB, D_REGION_USING_BLOCKS, 189},    //
          bin_config_t{D_ALLOC_SIZE(56, 0, 0), D_CHUNK_64MiB, D_REGION_USING_BLOCKS, 190},    //
          bin_config_t{D_ALLOC_SIZE(60, 0, 0), D_CHUNK_64MiB, D_REGION_USING_BLOCKS, 191},    //
          bin_config_t{D_ALLOC_SIZE(64, 0, 0), D_CHUNK_64MiB, D_REGION_USING_BLOCKS, 192},    //
          bin_config_t{D_ALLOC_SIZE(72, 0, 0), D_CHUNK_128MiB, D_REGION_USING_BLOCKS, 193},   //
          bin_config_t{D_ALLOC_SIZE(80, 0, 0), D_CHUNK_128MiB, D_REGION_USING_BLOCKS, 194},   //
          bin_config_t{D_ALLOC_SIZE(88, 0, 0), D_CHUNK_128MiB, D_REGION_USING_BLOCKS, 195},   //
          bin_config_t{D_ALLOC_SIZE(96, 0, 0), D_CHUNK_128MiB, D_REGION_USING_BLOCKS, 196},   //
          bin_config_t{D_ALLOC_SIZE(104, 0, 0), D_CHUNK_128MiB, D_REGION_USING_BLOCKS, 197},  //
          bin_config_t{D_ALLOC_SIZE(112, 0, 0), D_CHUNK_128MiB, D_REGION_USING_BLOCKS, 198},  //
          bin_config_t{D_ALLOC_SIZE(120, 0, 0), D_CHUNK_128MiB, D_REGION_USING_BLOCKS, 199},  //
          bin_config_t{D_ALLOC_SIZE(128, 0, 0), D_CHUNK_128MiB, D_REGION_USING_BLOCKS, 200},  //
          bin_config_t{D_ALLOC_SIZE(144, 0, 0), D_CHUNK_256MiB, D_REGION_USING_BLOCKS, 201},  //
          bin_config_t{D_ALLOC_SIZE(160, 0, 0), D_CHUNK_256MiB, D_REGION_USING_BLOCKS, 202},  //
          bin_config_t{D_ALLOC_SIZE(176, 0, 0), D_CHUNK_256MiB, D_REGION_USING_BLOCKS, 203},  //
          bin_config_t{D_ALLOC_SIZE(192, 0, 0), D_CHUNK_256MiB, D_REGION_USING_BLOCKS, 204},  //
          bin_config_t{D_ALLOC_SIZE(208, 0, 0), D_CHUNK_256MiB, D_REGION_USING_BLOCKS, 205},  //
          bin_config_t{D_ALLOC_SIZE(224, 0, 0), D_CHUNK_256MiB, D_REGION_USING_BLOCKS, 206},  //
          bin_config_t{D_ALLOC_SIZE(240, 0, 0), D_CHUNK_256MiB, D_REGION_USING_BLOCKS, 207},  //
          bin_config_t{D_ALLOC_SIZE(256, 0, 0), D_CHUNK_256MiB, D_REGION_USING_BLOCKS, 208},  //
          bin_config_t{D_ALLOC_SIZE(288, 0, 0), D_CHUNK_512MiB, D_REGION_USING_BLOCKS, 209},  //
          bin_config_t{D_ALLOC_SIZE(320, 0, 0), D_CHUNK_512MiB, D_REGION_USING_BLOCKS, 210},  //
          bin_config_t{D_ALLOC_SIZE(352, 0, 0), D_CHUNK_512MiB, D_REGION_USING_BLOCKS, 211},  //
          bin_config_t{D_ALLOC_SIZE(384, 0, 0), D_CHUNK_512MiB, D_REGION_USING_BLOCKS, 212},  //
          bin_config_t{D_ALLOC_SIZE(416, 0, 0), D_CHUNK_512MiB, D_REGION_USING_BLOCKS, 213},  //
          bin_config_t{D_ALLOC_SIZE(448, 0, 0), D_CHUNK_512MiB, D_REGION_USING_BLOCKS, 214},  //
          bin_config_t{D_ALLOC_SIZE(480, 0, 0), D_CHUNK_512MiB, D_REGION_USING_BLOCKS, 215},  //
          bin_config_t{D_ALLOC_SIZE(512, 0, 0), D_CHUNK_512MiB, D_REGION_USING_BLOCKS, 216},  //
        };

        struct calloc_t
        {
            byte*          m_address_base;                     // base address of the superallocator
            int_t          m_address_size;                     // size of the address space
            arena_t*       m_internal_heap;                    // internal heap for initialization allocations
            u8             m_segment_size_shift;               // size of each segment (e.g. 1 GiB)
            u32            m_segment_address_count;            // number of address segments in the whole address space
            u16*           m_address_segments;                 // accross the whole address space (heap)
            u64            m_address_segments_free_bin0;       // binmap bin0 of free address segments
            u64*           m_address_segments_free_bin1;       // binmap bin1 of free address segments (heap)
            size_to_bin_fn m_size_to_bin;                      // function to map size to bin index
            bin_config_t*  m_bin_configs;                      // bin configurations
            u16*           m_active_segment_per_region_index;  // active segment-lists per region index (heap)
            u32*           m_active_region_per_region_index;   // active region-lists per region index (heap)
            u32*           m_active_chunk_per_alloc_size;      // active chunk-lists per allocation size (heap)
            u32            m_page_size;                        // system page size
            u32            m_chunk_capacity;                   // maximum number of chunks
            u32            m_chunk_count;                      // current number of chunks
            u32            m_region_capacity;                  // maximum number of regions
            u32            m_region_count;                     // current number of regions
            u16            m_segment_capacity;                 // maximum number of segments
            u16            m_segment_count;                    // current number of segments
            chunk_t*       m_chunk_free_list;                  // free list of chunks
            chunk_t*       m_chunk_array;                      // array of chunks (heap)
            region_t*      m_region_free_list;                 // free list of regions
            region_t*      m_region_array;                     // array of regions (heap)
            segment_t*     m_segment_free_list;                // free list of segments
            segment_t*     m_segment_array;                    // array of segments (heap)
        };

        static inline u32 chunk_index(calloc_t* c, chunk_t* chunk) { return (u32)(chunk - c->m_chunk_array); }
        static inline u32 region_index(calloc_t* c, region_t* r) { return (u32)(r - c->m_region_array); }
        static inline u32 segment_index(calloc_t* c, segment_t* s) { return (u32)(s - c->m_segment_array); }

        chunk_t* new_chunk(calloc_t* c)
        {
            if (c->m_chunk_free_list != nullptr)
            {
                chunk_t* chunk = c->m_chunk_free_list;
                if (!is_nil(chunk->m_chunk_next))
                {
                    c->m_chunk_free_list               = &c->m_chunk_array[chunk->m_chunk_next];
                    c->m_chunk_free_list->m_chunk_prev = nu32::NIL;
                }
                else
                {
                    c->m_chunk_free_list = nullptr;
                }
                chunk->m_chunk_next = nu32::NIL;
                chunk->m_chunk_prev = nu32::NIL;
                return chunk;
            }
            else if (c->m_chunk_count < c->m_chunk_capacity)
            {
                chunk_t* chunk = &c->m_chunk_array[c->m_chunk_count++];
                return chunk;
            }
            return nullptr;
        }

        region_t* new_region(calloc_t* c)
        {
            region_t* region = c->m_region_free_list;
            if (region != nullptr)
            {
                if (!is_nil(region->m_next))
                {
                    c->m_region_free_list         = &c->m_region_array[region->m_next];
                    c->m_region_free_list->m_prev = nu32::NIL;
                }
                else
                {
                    c->m_region_free_list = nullptr;
                }
            }
            else if (c->m_region_count < c->m_region_capacity)
            {
                region = &c->m_region_array[c->m_region_count++];
            }
            else
            {
                return nullptr;
            }
            region->m_free_map      = 0;
            region->m_array         = nullptr;
            region->m_region_usage  = 0;
            region->m_segment_index = 0;
            region->m_region_index  = 0;
            region->m_next          = nu32::NIL;
            region->m_prev          = nu32::NIL;
            return nullptr;
        }

        region_t* new_region(calloc_t* c, segment_t* s, const bin_config_t& bin_config)
        {
            // get a new region from this segment

            // config_t m_config;          // region configuration for this segment
            // u16          m_segment_address_index;  // address index of this segment
            // u16          m_region_count;           // current number of regions for this segment
            // u16          m_region_capacity;        // maximum number of regions for this segment
            // u64          m_region_free_bin0;       // binmap bin0 of free regions for this segment
            // u64*         m_region_free_bin1;       // binmap bin1 of free regions for this segment
            // u32*         m_region_array;           // array of regions for this segment
            ASSERT(s->m_region_count < s->m_region_capacity);
            const i32 region_index = nbinmap6::find_and_set(&s->m_region_free_bin0, s->m_region_capacity);
            if (region_index >= 0)
            {
                u32*      region_item   = &s->m_region_array[region_index];
                region_t* region        = new_region(c);
                region->m_segment_index = (u16)(segment_index(c, s));
                if (bin_config.m_region_usage == D_REGION_USING_CHUNKS)
                {
                    region->m_array = nullptr;
                }
                else
                {
                    region->m_array = nullptr;
                }

                return region;
            }

            return nullptr;
        }

        segment_t* new_segment(calloc_t* c, const config_t& region_config)
        {
            if (c->m_segment_free_list != nullptr)
            {
                segment_t* segment = c->m_segment_free_list;
                if (!is_nil(segment->m_next))
                {
                    c->m_segment_free_list         = &c->m_segment_array[segment->m_next];
                    c->m_segment_free_list->m_prev = nu32::NIL;
                }
                else
                {
                    c->m_segment_free_list = nullptr;
                }
                return segment;
            }
            else if (c->m_segment_count < c->m_segment_capacity)
            {
                segment_t* segment = &c->m_segment_array[c->m_segment_count++];
                segment->m_config  = region_config;
                return segment;
            }
            return nullptr;
        }

        region_t* get_region(calloc_t* c, const bin_config_t& bin_config)
        {
            // see if we have an active region for this config
            u32 active_region_index = c->m_active_region_per_region_index[bin_config.m_config.get_index()];
            if (!is_nil(active_region_index))
            {
                return &c->m_region_array[active_region_index];
            }

            u32 active_segment_index = c->m_active_segment_per_region_index[bin_config.m_config.get_index()];
            if (is_nil(active_segment_index))
            {
                // get a new segment
            }

            return nullptr;
        }

        chunk_t* get_chunk_from_region(calloc_t* c, region_t* region, const bin_config_t& bin_config)
        {
            const i32 chunk_index = nbinmap6::find_and_set(&region->m_free_map, sizeof(region->m_free_map) * 8);
            if (chunk_index < 0)
                return nullptr;

            chunk_t* chunk = new_chunk(c);
            if (chunk != nullptr)
            {
                chunk->m_region_index       = (i32)(region - c->m_region_array);
                chunk->m_region_chunk_index = (u32)chunk_index;
                chunk->m_shift_size         = bin_config.m_config.get_chunk_size_shift();

                // i8   m_region_index;        // index into region array (back reference)
                // i8   m_region_chunk_index;  // index of the chunk in the region (back reference)
                // i8   m_shift_size;          // shift size of the chunk (1 << shift_size = chunk size)
                // i8   m_bin_levels;          // number of bin levels in the binmap for free elements
                // u32  m_pages_committed;     // number of committed pages in the chunk
                // u32  m_element_size;        // size of each element in the chunk
                // u32  m_element_capacity;    // number of elements in the chunk
                // u32  m_element_count;       // number of elements in the chunk
                // u32  m_element_free_index;  // current element free index
                // u64  m_bin0;                // binmap, level 0 (6)
                // u64* m_bin1;                // binmap, level 1 (6)
                // u32  m_chunk_prev;          // previous chunk in the list
                // u32  m_chunk_next;          // next chunk in the list

                return chunk;
            }

            return nullptr;
        }

        chunk_t* get_active_chunk(calloc_t* c, const bin_config_t& bin_config)
        {
            const u32 active_chunk_index = c->m_active_chunk_per_alloc_size[bin_config.m_bin_index];
            if (!is_nil(active_chunk_index))
            {
                return &c->m_chunk_array[active_chunk_index];
            }
            return nullptr;
        }

        void add_chunk_to_active_list(calloc_t* c, const bin_config_t& bin_config, chunk_t* chunk)
        {
            ASSERT(is_nil(chunk->m_chunk_prev) && is_nil(chunk->m_chunk_next));
            const u32 chunk_index                                    = (chunk - c->m_chunk_array);
            const u32 list_head                                      = c->m_active_chunk_per_alloc_size[bin_config.m_bin_index];
            chunk->m_chunk_prev                                      = nu32::NIL;
            chunk->m_chunk_next                                      = list_head;
            c->m_active_chunk_per_alloc_size[bin_config.m_bin_index] = chunk_index;
        }

        void remove_chunk_from_active_list(calloc_t* c, const bin_config_t& bin_config, chunk_t* chunk)
        {
            const u32 chunk_index = (chunk - c->m_chunk_array);
            u32&      list_head   = c->m_active_chunk_per_alloc_size[bin_config.m_bin_index];
            if (list_head == chunk_index)
            {
                list_head = chunk->m_chunk_next;
                if (!is_nil(list_head))
                {
                    chunk_t* head_chunk      = &c->m_chunk_array[list_head];
                    head_chunk->m_chunk_prev = nu32::NIL;
                }
            }
            else
            {
                ASSERT(!is_nil(chunk->m_chunk_prev));  // sanity check
                chunk_t* prev_chunk      = &c->m_chunk_array[chunk->m_chunk_prev];
                prev_chunk->m_chunk_next = chunk->m_chunk_next;
                if (!is_nil(chunk->m_chunk_next))
                {
                    chunk_t* next_chunk      = &c->m_chunk_array[chunk->m_chunk_next];
                    next_chunk->m_chunk_prev = chunk->m_chunk_prev;
                }
            }
            chunk->m_chunk_prev = nu32::NIL;
            chunk->m_chunk_next = nu32::NIL;
        }

        // Allocate memory of given size
        void* alloc(calloc_t* c, u32 size)
        {
            const u8            bin_index  = c->m_size_to_bin(size);
            const bin_config_t& bin_config = c->m_bin_configs[bin_index];
            if (c->m_active_chunk_per_alloc_size[bin_index] == 0)
            {
                region_t* region = get_region(c, bin_config);
                if (region == nullptr)
                    return nullptr;
                if (bin_config.m_region_usage == D_REGION_USING_BLOCKS)
                {
                    // TODO, allocate a block from a region and make sure enough pages are committed
                    return nullptr;
                }
                chunk_t* chunk = get_chunk_from_region(c, region, bin_config);
                if (chunk == nullptr)
                    return nullptr;
                add_chunk_to_active_list(c, bin_config, chunk);
            }
            chunk_t*   chunk         = get_active_chunk(c, bin_config);
            region_t*  region        = &c->m_region_array[chunk->m_region_index];
            segment_t* segment       = &c->m_segment_array[region->m_segment_index];
            byte*      chunk_address = (byte*)((int_t)segment->m_segment_address_index << c->m_segment_size_shift);
            chunk_address += (region->m_region_index << segment->m_config.get_region_size_shift());
            chunk_address += (chunk->m_region_chunk_index << segment->m_config.get_chunk_size_shift());
            void* ptr = alloc_from_chunk(chunk, chunk_address);
            if (chunk_is_full(chunk))
            {
                remove_chunk_from_active_list(c, bin_config, chunk);
            }
            return ptr;
        }

        void dealloc(calloc_t* c, void* ptr)
        {
            const u32 segment_address_index = (u32)(((byte*)ptr - c->m_address_base) / c->m_segment_size_shift);
            ASSERTS(segment_address_index >= 0 && segment_address_index < (c->m_address_size >> c->m_segment_size_shift), "invalid segment index");
            const u16 segment_index = c->m_address_segments[segment_address_index];
            ASSERTS(segment_index >= 0 && segment_index < c->m_segment_count, "invalid segment index");
            segment_t*  segment         = &c->m_segment_array[segment_index];
            const byte* segment_address = c->m_address_base + ((int_t)segment_index << c->m_segment_size_shift);

            const u32 region_index = (u32)(((const byte*)ptr - segment_address) >> segment->m_config.get_region_size_shift());
            ASSERTS(region_index >= 0 && region_index < segment->m_region_count, "invalid region index");
            const u32 region_entry = segment->m_region_array[region_index];
            ASSERTS(is_nil(region_entry), "region not allocated");
            region_t*   region         = &c->m_region_array[region_entry];
            const byte* region_address = segment_address + (region_index << segment->m_config.get_region_size_shift());
            if (region->m_region_usage == D_REGION_USING_BLOCKS)
            {
                const u32 block_index = ((const byte*)ptr - region_address) >> segment->m_config.get_chunk_size_shift();
                // TODO, deallocating from a block:
                // - validate address within region
                // - calculate block index
                // - mark block as free
                // - if required, decommit pages
            }
            else
            {
                const u32 chunk_index = ((const byte*)ptr - region_address) >> segment->m_config.get_chunk_size_shift();
                const u32 chunk_entry = region->m_array[chunk_index];
                ASSERTS(chunk_entry != nu32::NIL, "chunk not allocated");
                chunk_t*    chunk         = &c->m_chunk_array[chunk_entry];
                const byte* chunk_address = region_address + (chunk_index << segment->m_config.get_chunk_size_shift());

                const bool chunk_full_before = chunk_is_full(chunk);
                switch (chunk->m_bin_levels)
                {
                    case 0: nbinmap6::clr(&chunk->m_bin0, chunk->m_element_free_index, chunk->m_element_count); break;
                    case 1: nbinmap12::clr(&chunk->m_bin0, chunk->m_bin1, chunk->m_element_free_index, chunk->m_element_count); break;
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
