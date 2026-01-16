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
#include "csuperalloc/c_fsa.h"

namespace ncore
{
    namespace nsuperallocv2
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

        struct alloc_config_t
        {
            u32 m_alloc_size;  // size of elements in this bin
            u8  m_region_size_shift;
            u8  m_chunk_size_shift;
            u8  m_block_size_shift;
            u8  m_region_index;

            inline u16 num_elements_per_chunk() const { return ((u32)1 << m_chunk_size_shift) / m_alloc_size; }
            inline u16 num_chunks_per_region() const { return (u16)((u32)1 << (m_region_size_shift - m_chunk_size_shift)); }
            inline u16 num_blocks_per_region() const { return (u16)((u32)1 << (m_region_size_shift - m_block_size_shift)); }

            void chunks(u16 mb, u16 kb, u16 b, u8 rss, u8 css)
            {
                m_alloc_size        = mb * 1024 * 1024 + kb * 1024 + b;
                m_region_index      = 0;
                m_region_size_shift = rss;
                m_chunk_size_shift  = css;
                m_block_size_shift  = 0;
            }
            void blocks(u16 mb, u16 kb, u16 b, u8 rss, u8 bss)
            {
                m_alloc_size        = mb * 1024 * 1024 + kb * 1024 + b;
                m_region_index      = 0;
                m_region_size_shift = rss;
                m_chunk_size_shift  = 0;
                m_block_size_shift  = bss;
            }
        };

#define D_CHUNK_REGION(index, region_size, chunk_size) {(u8)index, (u8)region_size, (u8)chunk_size, (u8)0}
#define D_BLOCK_REGION(index, region_size, block_size) {(u8)index, (u8)region_size, (u8)0, (u8)block_size}
#define D_MKB(mb, kb, b)                               ((mb * 1024 * 1024) + (kb * 1024) + b)

        static u8 size2bin(u32 alloc_size)
        {
            alloc_size  = alloc_size < 16 ? 16 : alloc_size;
            const s8  u = math::ilog2(alloc_size);
            const s8  d = u - 2;
            const u32 i = (alloc_size - ((u32)1 << u) + ((u32)1 << d) - 1) >> d;
            return ((u8)(u - 4) * 4) + (u8)i;
        }

#define D_MAX_SEGMENTS           1024
#define D_MAX_REGIONS            65535
#define D_MAX_CHUNKS_PER_REGION  512
#define D_MAX_BLOCKS_PER_REGION  512
#define D_MAX_ELEMENTS_PER_CHUNK 1024

        // A chunk consists of N elements with a maximum of 1024.
        // 16 bytes
        struct chunk_t
        {
            u16 m_pages;       // number of committed pages in the chunk
            u16 m_capacity;    // number of elements in the chunk
            u16 m_count;       // number of elements in the chunk
            u16 m_free_index;  // current element free index
            u32 m_free_bin0;   // binmap, level 0 (5)
            u32 m_free_bin1;   // binmap, level 1 (5) (fsa, idx2ptr, max 128 bytes)
        };

        // A region consists of N blocks
        // A block is a fixed size number of pages (power-of-two number of pages).
        // But an allocation can use less pages than the block size, so we need to track
        // the number of committed pages per block.
        struct block_t
        {
            u32 m_pages;  // number of committed pages in the block
        };

        typedef u8 (*size_to_bin_fn)(u32 size);

        // A region consists of N chunks/blocks
        // Chunks in a region are of the same size (chunk_size_shift).
        // A region is dedicated to a specific index (alloc config).
        struct region_t
        {
            u16 m_chunk_free_index;   // index of the first free chunk/block in the region
            u16 m_chunk_count;        // number of chunks/blocks in use
            u8  m_alloc_index;        // the alloc config index for this region
            u8  m_local_index;        // the index of this region in the region array of segment
            u16 m_segment_index;      // the segment index this region belongs to
            u32 m_chunk_array;        // array of chunk/block indices (fsa, idx2ptr)
            u32 m_chunk_free_bin0;    // free chunks binmap, level 0
            u32 m_chunk_free_bin1;    // free chunks binmap, level 1 (fsa, idx2ptr)
            u32 m_chunk_active_bin0;  // active chunks binmap, level 0
            u32 m_chunk_active_bin1;  // active chunks binmap, level 1 (fsa, idx2ptr)
            u32 m_next;               // next region in list
        };

        // A segment consists of N regions
        // 16 bytes
        struct segment_t
        {
            u32 m_region_free_list;   // binmap level 0 of free regions
            u32 m_region_array;       // array of region indices (fsa, idx2ptr)
            u8  m_region_free_index;  // index of first free region in this segment
            u8  m_region_size_shift;  // size of regions in this segment
            u8  m_region_capacity;    // maximum number of regions in this segment
            u8  m_region_count;       // number of used regions in this segment
            u16 m_next;               // segment list
            u16 m_prev;               // segment list
        };

        struct calloc_t
        {
            byte*  m_address_base;         // base address of the superallocator
            int_t  m_address_size;         // size of the address space
            fsa_t* m_internal_fsa;         // internal fsa for runtime allocations
            byte*  m_regions_base;         // base address of region metadata (N * 16KiB)
            byte*  m_allocations_base;     // base address of allocations
            u8     m_allocator_num_pages;  // number of pages used by the allocator itself
            u8     m_region_num_pages;     // number of pages per region
            u8     m_segment_size_shift;   // size of each segment (e.g. 1 GiB)
            u8     m_page_size_shift;      // system page size

            // alloc_config_t
            u32             m_num_alloc_configs;  // number of bin configurations
            size_to_bin_fn  m_size_to_bin;        // function to map size to bin index
            alloc_config_t* m_alloc_configs;      // bin configurations

            // regions
            region_t* m_regions;                   // the (virtual) memory where regions are allocate from
            u32       m_regions_free_list;         // head of free region struct list
            u32       m_regions_free_index;        // index to the first free region struct
            u32       m_regions_count;             // number of region structs in use
            u32       m_regions_capacity;          // number of region structs allocated
            u32*      m_active_regions_per_index;  // active regions per index

            // segments
            u16        m_segments_free_list;              // head of free segment list
            u32        m_segments_free_index;             // index to the first free segment
            u32        m_segments_capacity;               // maximum number of segments
            u32        m_segments_count;                  // number of segments in use
            segment_t* m_segments;                        // segment array (pre-allocated), this is where we allocate segment_t from
            u16*       m_active_segment_per_region_size;  // active segments per region size
        };

        static void initializeActiveRegionsPerBin(calloc_t* c, byte*& allocator_mem)
        {
            c->m_active_regions_per_index = (u32*)allocator_mem;
            allocator_mem += sizeof(u32) * 128;
        }

        static void initializeSegments(calloc_t* c, byte*& allocator_mem)
        {
            u8 const  segment_size_shift = 30;  // 1 GiB segments
            u32 const segment_count      = (u32)(c->m_address_size >> segment_size_shift);
            c->m_segments                = (segment_t*)allocator_mem;
            allocator_mem += sizeof(segment_t) * segment_count;
            c->m_segments_free_list             = 0xFFFF;
            c->m_segments_free_index            = 0;
            c->m_active_segment_per_region_size = (u16*)allocator_mem;
            allocator_mem += sizeof(u16) * 16;
        }

        static void initializeRegions(calloc_t* c, byte*& allocator_mem)
        {
            u8 const  segment_size_shift  = 30;  // 1 GiB segments
            u32 const segment_count       = (u32)(c->m_address_size >> segment_size_shift);
            u32 const regions_per_segment = 32;  // 32 regions of 32 MiB in 1 GiB segment

            c->m_regions = (region_t*)((byte*)c + ((u64)2 << c->m_page_size_shift));
            // commit the first page of the regions memory
            v_alloc_commit(c->m_regions, (u64)1 << c->m_page_size_shift);

            c->m_active_regions_per_index = (u32*)allocator_mem;
            allocator_mem += sizeof(u32) * 128;
        }

        static void initializeAllocConfigs(calloc_t* c, byte*& allocator_mem)
        {
            c->m_alloc_configs = (alloc_config_t*)allocator_mem;

            i32 i = 0;
            c->m_alloc_configs[i++].chunks(0, 0, 16, c8MiB, c16KiB);     // 16
            c->m_alloc_configs[i++].chunks(0, 0, 32, c8MiB, c16KiB);     // 20
            c->m_alloc_configs[i++].chunks(0, 0, 32, c8MiB, c16KiB);     // 24
            c->m_alloc_configs[i++].chunks(0, 0, 32, c8MiB, c16KiB);     // 28
            c->m_alloc_configs[i++].chunks(0, 0, 32, c16MiB, c32KiB);    // 32
            c->m_alloc_configs[i++].chunks(0, 0, 48, c16MiB, c32KiB);    // 40
            c->m_alloc_configs[i++].chunks(0, 0, 48, c16MiB, c32KiB);    // 48
            c->m_alloc_configs[i++].chunks(0, 0, 64, c16MiB, c32KiB);    // 56
            c->m_alloc_configs[i++].chunks(0, 0, 64, c32MiB, c64KiB);    // 64
            c->m_alloc_configs[i++].chunks(0, 0, 80, c32MiB, c64KiB);    // 80
            c->m_alloc_configs[i++].chunks(0, 0, 96, c32MiB, c64KiB);    // 96
            c->m_alloc_configs[i++].chunks(0, 0, 112, c32MiB, c64KiB);   // 112
            c->m_alloc_configs[i++].chunks(0, 0, 128, c32MiB, c64KiB);   // 128
            c->m_alloc_configs[i++].chunks(0, 0, 160, c32MiB, c64KiB);   // 160
            c->m_alloc_configs[i++].chunks(0, 0, 192, c32MiB, c64KiB);   // 192
            c->m_alloc_configs[i++].chunks(0, 0, 224, c32MiB, c64KiB);   // 224
            c->m_alloc_configs[i++].chunks(0, 0, 256, c32MiB, c64KiB);   // 256
            c->m_alloc_configs[i++].chunks(0, 0, 320, c32MiB, c64KiB);   // 320
            c->m_alloc_configs[i++].chunks(0, 0, 384, c32MiB, c64KiB);   // 384
            c->m_alloc_configs[i++].chunks(0, 0, 448, c32MiB, c64KiB);   // 448
            c->m_alloc_configs[i++].chunks(0, 0, 512, c32MiB, c64KiB);   // 512
            c->m_alloc_configs[i++].chunks(0, 0, 640, c32MiB, c64KiB);   // 640
            c->m_alloc_configs[i++].chunks(0, 0, 768, c32MiB, c64KiB);   // 768
            c->m_alloc_configs[i++].chunks(0, 0, 896, c32MiB, c64KiB);   // 896
            c->m_alloc_configs[i++].chunks(0, 1, 0, c32MiB, c64KiB);     // 1KB
            c->m_alloc_configs[i++].chunks(0, 1, 256, c32MiB, c64KiB);   //
            c->m_alloc_configs[i++].chunks(0, 1, 512, c32MiB, c64KiB);   //
            c->m_alloc_configs[i++].chunks(0, 1, 768, c32MiB, c64KiB);   //
            c->m_alloc_configs[i++].chunks(0, 2, 0, c32MiB, c64KiB);     //
            c->m_alloc_configs[i++].chunks(0, 2, 512, c32MiB, c64KiB);   //
            c->m_alloc_configs[i++].chunks(0, 3, 0, c32MiB, c64KiB);     //
            c->m_alloc_configs[i++].chunks(0, 3, 512, c32MiB, c64KiB);   //
            c->m_alloc_configs[i++].chunks(0, 4, 0, c32MiB, c64KiB);     //
            c->m_alloc_configs[i++].chunks(0, 5, 0, c32MiB, c64KiB);     //
            c->m_alloc_configs[i++].chunks(0, 6, 0, c32MiB, c64KiB);     //
            c->m_alloc_configs[i++].chunks(0, 7, 0, c32MiB, c64KiB);     //
            c->m_alloc_configs[i++].chunks(0, 8, 0, c32MiB, c64KiB);     //
            c->m_alloc_configs[i++].chunks(0, 10, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].chunks(0, 12, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].chunks(0, 14, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].chunks(0, 16, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].chunks(0, 20, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].chunks(0, 24, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].chunks(0, 28, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].chunks(0, 32, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].blocks(0, 40, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].blocks(0, 48, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].blocks(0, 56, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].blocks(0, 64, 0, c32MiB, c64KiB);    //
            c->m_alloc_configs[i++].blocks(0, 80, 0, c32MiB, c128KiB);   //
            c->m_alloc_configs[i++].blocks(0, 96, 0, c32MiB, c128KiB);   //
            c->m_alloc_configs[i++].blocks(0, 112, 0, c32MiB, c128KiB);  //
            c->m_alloc_configs[i++].blocks(0, 128, 0, c32MiB, c128KiB);  //
            c->m_alloc_configs[i++].blocks(0, 160, 0, c32MiB, c256KiB);  //
            c->m_alloc_configs[i++].blocks(0, 192, 0, c32MiB, c256KiB);  //
            c->m_alloc_configs[i++].blocks(0, 224, 0, c32MiB, c256KiB);  //
            c->m_alloc_configs[i++].blocks(0, 256, 0, c32MiB, c256KiB);  //
            c->m_alloc_configs[i++].blocks(0, 320, 0, c32MiB, c512KiB);  //
            c->m_alloc_configs[i++].blocks(0, 384, 0, c32MiB, c512KiB);  //
            c->m_alloc_configs[i++].blocks(0, 448, 0, c32MiB, c512KiB);  //
            c->m_alloc_configs[i++].blocks(0, 512, 0, c32MiB, c512KiB);  //
            c->m_alloc_configs[i++].blocks(0, 640, 0, c32MiB, c1MiB);    //
            c->m_alloc_configs[i++].blocks(0, 768, 0, c32MiB, c1MiB);    //
            c->m_alloc_configs[i++].blocks(0, 896, 0, c32MiB, c1MiB);    //
            c->m_alloc_configs[i++].blocks(1, 0, 0, c32MiB, c1MiB);      //
            c->m_alloc_configs[i++].blocks(1, 256, 0, c32MiB, c2MiB);    //
            c->m_alloc_configs[i++].blocks(1, 512, 0, c32MiB, c2MiB);    //
            c->m_alloc_configs[i++].blocks(1, 768, 0, c32MiB, c2MiB);    //
            c->m_alloc_configs[i++].blocks(2, 0, 0, c32MiB, c2MiB);      //
            c->m_alloc_configs[i++].blocks(2, 512, 0, c32MiB, c4MiB);    //
            c->m_alloc_configs[i++].blocks(3, 0, 0, c32MiB, c4MiB);      //
            c->m_alloc_configs[i++].blocks(3, 512, 0, c32MiB, c4MiB);    //
            c->m_alloc_configs[i++].blocks(4, 0, 0, c32MiB, c4MiB);      //
            c->m_alloc_configs[i++].blocks(5, 0, 0, c32MiB, c8MiB);      //
            c->m_alloc_configs[i++].blocks(6, 0, 0, c32MiB, c8MiB);      //
            c->m_alloc_configs[i++].blocks(7, 0, 0, c32MiB, c8MiB);      //
            c->m_alloc_configs[i++].blocks(8, 0, 0, c32MiB, c8MiB);      //
            c->m_alloc_configs[i++].blocks(10, 0, 0, c32MiB, c16MiB);    //
            c->m_alloc_configs[i++].blocks(12, 0, 0, c32MiB, c16MiB);    //
            c->m_alloc_configs[i++].blocks(14, 0, 0, c32MiB, c16MiB);    //
            c->m_alloc_configs[i++].blocks(16, 0, 0, c32MiB, c16MiB);    //
            c->m_alloc_configs[i++].blocks(20, 0, 0, c32MiB, c32MiB);    //
            c->m_alloc_configs[i++].blocks(24, 0, 0, c32MiB, c32MiB);    //
            c->m_alloc_configs[i++].blocks(28, 0, 0, c32MiB, c32MiB);    //
            c->m_alloc_configs[i++].blocks(32, 0, 0, c32MiB, c32MiB);    //
            c->m_alloc_configs[i++].blocks(40, 0, 0, c32MiB, c64MiB);    //
            c->m_alloc_configs[i++].blocks(48, 0, 0, c32MiB, c64MiB);    //
            c->m_alloc_configs[i++].blocks(56, 0, 0, c32MiB, c64MiB);    //
            c->m_alloc_configs[i++].blocks(64, 0, 0, c32MiB, c64MiB);    //
            c->m_alloc_configs[i++].blocks(80, 0, 0, c32MiB, c128MiB);   //
            c->m_alloc_configs[i++].blocks(96, 0, 0, c32MiB, c128MiB);   //
            c->m_alloc_configs[i++].blocks(112, 0, 0, c32MiB, c128MiB);  //
            c->m_alloc_configs[i++].blocks(128, 0, 0, c32MiB, c128MiB);  //
            c->m_alloc_configs[i++].blocks(160, 0, 0, c32MiB, c256MiB);  //
            c->m_alloc_configs[i++].blocks(192, 0, 0, c32MiB, c256MiB);  //
            c->m_alloc_configs[i++].blocks(224, 0, 0, c32MiB, c256MiB);  //
            c->m_alloc_configs[i++].blocks(256, 0, 0, c32MiB, c256MiB);  //
            c->m_alloc_configs[i++].blocks(320, 0, 0, c32MiB, c512MiB);  //
            c->m_alloc_configs[i++].blocks(384, 0, 0, c32MiB, c512MiB);  //
            c->m_alloc_configs[i++].blocks(448, 0, 0, c32MiB, c512MiB);  //
            c->m_alloc_configs[i++].blocks(512, 0, 0, c32MiB, c512MiB);  //
            c->m_num_alloc_configs = i;

            u8 region_index = 0;
            for (i32 j = 1; j < i; ++j)
            {
                if (j > 0)
                {
                    if (c->m_alloc_configs[j].m_block_size_shift == 0)
                    {
                        if (c->m_alloc_configs[j].m_alloc_size != c->m_alloc_configs[j - 1].m_alloc_size)
                            region_index += 1;
                    }
                    else
                    {
                        if (c->m_alloc_configs[j].m_block_size_shift != c->m_alloc_configs[j - 1].m_block_size_shift)
                            region_index += 1;
                    }
                }
                c->m_alloc_configs[j].m_region_index = region_index;
            }

            allocator_mem += sizeof(alloc_config_t) * i;
        }

        inline bool chunk_is_full(chunk_t* chunk) { return chunk->m_count == chunk->m_capacity; }
        inline bool chunk_is_empty(chunk_t* chunk) { return chunk->m_count == 0; }

        inline byte* region_bookkeeping_data(calloc_t* c, u32 region_index)
        {
            byte* array = (byte*)c;
            c += (c->m_allocator_num_pages << c->m_page_size_shift);
            c += ((region_index * c->m_region_num_pages) << c->m_page_size_shift);
            return array;
        }

        inline bool region_is_block_based(calloc_t* c, region_t* region)
        {
            const alloc_config_t& config = c->m_alloc_configs[region->m_alloc_index];
            return config.m_block_size_shift > 0;
        }

        inline chunk_t* region_chunk_array(calloc_t* c, u32 region_index)
        {
            byte* data = region_bookkeeping_data(c, region_index);
            data += 32 * sizeof(u32);  // skip free chunk bin1
            data += 32 * sizeof(u32);  // skip active chunk bin1
            return (chunk_t*)data;
        }

        inline chunk_t* region_chunk(calloc_t* c, region_t* region, u32 chunk_index)
        {
            const u32 region_index = (region - c->m_regions);
            chunk_t*  chunk_array  = region_chunk_array(c, region_index);
            return &chunk_array[chunk_index];
        }

        inline u32 region_chunk_index(calloc_t* c, region_t* region, chunk_t* chunk)
        {
            const u32 region_index = (region - c->m_regions);
            chunk_t*  chunk_array  = region_chunk_array(c, region_index);
            return (u32)(chunk - chunk_array);
        }

        static void* alloc_from_chunk(calloc_t* c, chunk_t* chunk, byte* chunk_address, u32 alloc_size)
        {
            ASSERT(chunk->m_count < chunk->m_capacity);
            i32 free_index;
            if (chunk->m_free_bin1 == 0xFFFFFFFF)
            {
                free_index = nbinmap5::find_and_set(&chunk->m_free_bin0, chunk->m_count);
            }
            else
            {
                u32* bin1  = (u32*)nfsa::idx2ptr(c->m_internal_fsa, chunk->m_free_bin1);
                free_index = nbinmap10::find_and_set(&chunk->m_free_bin0, bin1, chunk->m_count);
            }
            if (free_index < 0)
                return nullptr;
            chunk->m_count += 1;
            return chunk_address + (free_index * alloc_size);
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
#define D_CHUNK_1GiB                  D_CHUNK_CFG(c1GiB)    //  1 GiB

#define D_ALLOC_SIZE(mb, kb, b) (((((u32)(mb)) & 0xFFFF) << 20) | ((((u32)(kb)) & 0x3FF) << 10) | (((u32)(b)) & 0x3FF))

#define D_USAGE_CHUNK      0
#define D_USAGE_BLOCK      1
#define D_CACHE_SIZE(size) ((u8)(size))

        struct region_config_t
        {
            u8 m_region_size_shift;  // size of region in main address space
            u8 m_chunk_size_shift;   // size of chunks/blocks in this region
            u8 m_block_size_shift;   // size of chunks/blocks in this region
        };

        static inline u16 get_segment_index(calloc_t* c, segment_t* segment) { return (u16)(segment - c->m_segments); }

        static inline segment_t* get_segment_at_index(calloc_t* c, u16 segment_index) { return &c->m_segments[segment_index]; }
        static inline segment_t* get_segment_for_region_size(calloc_t* c, u8 region_size_shift)
        {
            const u32 segment_index = c->m_active_segment_per_region_size[region_size_shift];
            if (segment_index == 0xFFFF)
                return nullptr;
            return &c->m_segments[segment_index];
        }
        static inline void add_segment_for_region_size(calloc_t* c, segment_t* segment)
        {
            u16&      list_head     = c->m_active_segment_per_region_size[segment->m_region_size_shift];
            const u16 segment_index = get_segment_index(c, segment);
            segment->m_next         = list_head;
            list_head               = segment_index;
        }

        static inline segment_t* allocate_segment(calloc_t* c, u8 region_size_shift)
        {
            i16 segment_index = 0xFFFF;
            if (c->m_segments_free_list != 0xFFFF)
            {
                segment_index           = c->m_segments_free_list;
                c->m_segments_free_list = c->m_segments[segment_index].m_next;
            }
            else if (c->m_segments_free_index < c->m_segments_capacity)
            {
                segment_index = c->m_segments_free_index++;
            }
            else
            {
                return nullptr;
            }

            segment_t* segment           = get_segment_at_index(c, segment_index);
            segment->m_region_size_shift = region_size_shift;
            segment->m_region_capacity   = 1 << (c->m_segment_size_shift - region_size_shift);
            segment->m_region_count      = 0;
            segment->m_region_free_list  = 0xFFFFFFFF;
            segment->m_region_free_index = 0;
            segment->m_next              = 0xFFFF;
            segment->m_prev              = 0xFFFF;

            return segment;
        }

        static inline u32       get_region_index(calloc_t* c, region_t* region) { return (u32)(region - c->m_regions); }
        static inline region_t* get_region_at_index(calloc_t* c, u32 region_index) { return &c->m_regions[region_index]; }
        static inline byte*     get_region_address(calloc_t* c, region_t* region)
        {
            const u16 segment_index        = region->m_segment_index;
            const u32 segment_region_index = region->m_local_index;
            byte*     address              = c->m_address_base + ((u64)segment_index << c->m_segment_size_shift);
            address += ((u64)segment_region_index << c->m_segments[segment_index].m_region_size_shift);
            return address;
        }

        static inline byte* get_region_chunk_address(calloc_t* c, region_t* region, u32 chunk_index)
        {
            byte* region_address = get_region_address(c, region);
            return region_address + ((int_t)chunk_index << c->m_alloc_configs[region->m_alloc_index].m_chunk_size_shift);
        }

        static inline region_t* get_active_region(calloc_t* c, alloc_config_t const& alloc_config)
        {
            // return the active region for this alloc index
            const u32 region_index = c->m_active_regions_per_index[alloc_config.m_region_index];
            if (region_index == 0xFFFF)
                return nullptr;
            return &c->m_regions[region_index];
        }

        static inline region_t* allocate_region(calloc_t* c, u8 alloc_index, segment_t* segment)
        {
            const u8 min_region_size_shift = 25;  // 32 MiB

            u32       region_index = 0xFFFFFFFF;
            region_t* region       = nullptr;
            if (c->m_regions_free_list != 0xFFFFFFFF)
            {
                // allocate from free list
                region_index           = c->m_regions_free_list;
                region                 = &c->m_regions[region_index];
                c->m_regions_free_list = region->m_next;
            }
            else if (c->m_regions_free_index < c->m_regions_capacity)
            {
                // allocate new region
                region_index = c->m_regions_free_index;
                region       = &c->m_regions[region_index];
                c->m_regions_free_index += 1;
                c->m_regions_count += 1;

                // TODO do we need to commit a page for region array ?
            }

            if (region != nullptr)
            {
                const alloc_config_t& alloc_config = c->m_alloc_configs[alloc_index];

                region->m_alloc_index       = alloc_index;
                region->m_next              = 0xFFFFFFFF;
                region->m_chunk_free_index  = 0;
                region->m_chunk_count       = 0;
                region->m_chunk_free_bin0   = 0;
                region->m_chunk_active_bin0 = 0;

                if (alloc_config.num_chunks_per_region() > 32)
                {
                    u32* free_bin1              = g_allocate_array<u32>(c->m_internal_fsa, (alloc_config.num_chunks_per_region() >> 5));
                    u32* active_bin1            = g_allocate_array<u32>(c->m_internal_fsa, (alloc_config.num_chunks_per_region() >> 5));
                    region->m_chunk_free_bin1   = nfsa::ptr2idx(c->m_internal_fsa, free_bin1);
                    region->m_chunk_active_bin1 = nfsa::ptr2idx(c->m_internal_fsa, active_bin1);
                }
                else
                {
                    region->m_chunk_free_bin1   = 0xFFFFFFFF;
                    region->m_chunk_active_bin1 = 0xFFFFFFFF;
                }

                // add region to segment
                u8 segment_region_idx = 0;

                if (segment->m_region_free_list != 0xFFFFFFFF)
                {
                    segment_region_idx          = (u8)segment->m_region_free_list;
                    segment->m_region_free_list = segment->m_region_free_index;
                    segment->m_region_count += 1;
                }
                else if (segment->m_region_free_index < segment->m_region_capacity)
                {
                    segment_region_idx = (u8)segment->m_region_free_index++;
                    segment->m_region_count += 1;
                }
                else
                {
                    ASSERT(false);  // segment has no more free regions?
                }

                ASSERT(segment_region_idx >= 0 && segment_region_idx < 256);
                region->m_local_index   = (u8)segment_region_idx;
                region->m_segment_index = get_segment_index(c, segment);
            }
            return region;
        }

        static inline void activate_region(calloc_t* c, region_t* region)
        {
            // set region properties
            const alloc_config_t& alloc_config = c->m_alloc_configs[region->m_alloc_index];

            region->m_alloc_index       = alloc_config.m_region_index;
            region->m_chunk_free_index  = 0;
            region->m_chunk_count       = 0;
            const u32 capacity          = alloc_config.num_chunks_per_region();
            region->m_chunk_free_bin0   = D_U32_MAX;
            region->m_chunk_active_bin0 = 0;

            // commit region memory
            const alloc_config_t& alloc_config        = c->m_alloc_configs[region->m_alloc_index];
            const u32             region_data_size    = (u32)(1 << 14);
            void*                 region_data_address = get_region_address(c, region);
            v_alloc_commit(region_data_address, region_data_size);
        }

        void add_region_to_active(calloc_t* c, region_t* region, const alloc_config_t& alloc_config)
        {
            const u32 region_index = get_region_index(c, region);
            if (c->m_active_regions_per_index[alloc_config.m_region_index] == 0xFFFFFFFF)
            {
                c->m_active_regions_per_index[alloc_config.m_region_index] = region_index;
            }
            else
            {
                const u32 head_index                                       = c->m_active_regions_per_index[alloc_config.m_region_index];
                region->m_next                                             = c->m_active_regions_per_index[alloc_config.m_region_index];
                c->m_active_regions_per_index[alloc_config.m_region_index] = region_index;
            }
        }

        region_t* pop_region_from_active(calloc_t* c, const alloc_config_t& alloc_config)
        {
            const u32 head_index = c->m_active_regions_per_index[alloc_config.m_region_index];
            if (head_index != 0xFFFFFFFF)
            {
                region_t* head_region                                      = &c->m_regions[head_index];
                c->m_active_regions_per_index[alloc_config.m_region_index] = head_region->m_next;
                head_region->m_next                                        = 0xFFFFFFFF;
                return head_region;
            }
            return nullptr;
        }

        static inline void release_region(calloc_t* c, region_t* region)
        {
            const u32  region_index         = get_region_index(c, region);
            const u32  segment_index        = region->m_segment_index;
            segment_t* segment              = &c->m_segments[segment_index];
            const u32  segment_region_index = region->m_local_index;
            if (segment->m_region_free_list == 0xFFFFFFFF)
            {
                segment->m_region_free_list = region_index;
                region->m_next              = 0xFFFFFFFF;
            }
            else
            {
                u32 region_head_index       = segment->m_region_free_list;
                segment->m_region_free_list = region_index;
                region->m_next              = region_head_index;
            }

            // TODO decommit region memory and release any other resources

        }

        static inline u32* get_region_free_chunk_bin1(calloc_t* c, region_t* region)
        {
            byte* region_data = region_bookkeeping_data(c, get_region_index(c, region));
            return (u32*)(region_data);
        }

        static inline u32* get_region_active_chunk_bin1(calloc_t* c, region_t* region)
        {
            byte* region_data = region_bookkeeping_data(c, get_region_index(c, region));
            return (u32*)(region_data + (sizeof(u32) * 32));
        }

        static inline chunk_t* allocate_chunk_from_region(calloc_t* c, region_t* region)
        {
            const alloc_config_t& alloc_config = c->m_alloc_configs[region->m_alloc_index];
            u32*      bin1  = get_region_free_chunk_bin1(c, region);
            const s32 index = nbinmap10::find_and_set(&region->m_chunk_free_bin0, bin1, alloc_config.num_chunks_per_region());
            if (index >= 0)
            {
                chunk_t* chunk = region_chunk(c, region, (u32)index);
                return chunk;
            }
            return nullptr;
        }

        static inline void activate_chunk(calloc_t* c, region_t* region, chunk_t* chunk)
        {
            const alloc_config_t& bincfg = c->m_alloc_configs[region->m_alloc_index];

            // initialize chunk
            chunk->m_capacity   = (1 << bincfg.m_chunk_size_shift) / bincfg.m_alloc_size;
            chunk->m_count      = 0;
            chunk->m_free_index = 0;
            chunk->m_pages      = ((int_t)1 << (bincfg.m_chunk_size_shift - c->m_page_size_shift));
            chunk->m_free_bin0  = D_U64_MAX << ((chunk->m_capacity + 63) >> 6);
            chunk->m_free_bin1  = 0xFFFFFFFF;
            if (chunk->m_capacity > 64)
            {
                u32* bin1          = (u32*)nfsa::allocate(c->m_internal_fsa, ((chunk->m_capacity + 31) >> 5) * sizeof(u32));
                chunk->m_free_bin1 = nfsa::ptr2idx(c->m_internal_fsa, bin1);
            }

            // TODO commit all pages for this chunk
            void* chunk_address = get_region_chunk_address(c, region, region_chunk_index(c, region, chunk));
            v_alloc_commit(chunk_address, chunk->m_pages << c->m_page_size_shift);
        }

        static inline void deactivate_chunk(calloc_t* c, chunk_t* chunk)
        {
            // TODO decommit all pages
            chunk->m_capacity   = 0;
            chunk->m_count      = 0;
            chunk->m_free_index = 0;
            chunk->m_pages      = 0;
            chunk->m_free_bin0  = 0;
            if (chunk->m_free_bin1 != 0xFFFFFFFF)
            {
                u32* bin1 = (u32*)nfsa::idx2ptr(c->m_internal_fsa, chunk->m_free_bin1);
                nfsa::deallocate(c->m_internal_fsa, bin1);
                chunk->m_free_bin1 = 0xFFFFFFFF;
            }
        }

        void release_chunk_to_region(calloc_t* c, region_t* region, chunk_t* chunk)
        {
            // todo
        }

        chunk_t* get_active_chunk(calloc_t* c, region_t* region)
        {
            const alloc_config_t& bincfg = c->m_alloc_configs[region->m_alloc_index];
            const u32* active_bin1 = get_region_active_chunk_bin1(c, region);
            s32 const  chunk_index = nbinmap10::find(&region->m_chunk_active_bin0, active_bin1, bincfg.num_chunks_per_region());
            if (chunk_index < 0)
                return nullptr;
            return region_chunk(c, region, (u32)chunk_index);
        }

        void add_chunk_to_active(calloc_t* c, region_t* region, chunk_t* chunk)
        {
            const alloc_config_t& bincfg = c->m_alloc_configs[region->m_alloc_index];
            const u32 chunk_index = region_chunk_index(c, region, chunk);
            u32*      active_bin1 = get_region_active_chunk_bin1(c, region);
            nbinmap10::set(&region->m_chunk_active_bin0, active_bin1, chunk_index, bincfg.num_chunks_per_region());
        }

        void remove_chunk_from_active(calloc_t* c,   region_t* region, chunk_t* chunk)
        {
            const alloc_config_t& bincfg = c->m_alloc_configs[region->m_alloc_index];
            const u32 chunk_index = region_chunk_index(c, region, chunk);
            u32*      active_bin1 = get_region_active_chunk_bin1(c, region);
            nbinmap10::clr(&region->m_chunk_active_bin0, active_bin1, chunk_index, bincfg.num_chunks_per_region());
        }

        // Allocate memory of given size
        void* alloc(calloc_t* c, u32 size)
        {
            const u8              alloc_index  = c->m_size_to_bin(size);
            const alloc_config_t& alloc_config = c->m_alloc_configs[alloc_index];
            region_t*             region       = get_active_region(c, alloc_config);
            if (region == nullptr)
            {
                segment_t* segment = get_segment_for_region_size(c, alloc_config.m_region_size_shift);
                if (segment == nullptr)
                {
                    segment_t* segment = allocate_segment(c, alloc_config.m_region_size_shift);
                    add_segment_for_region_size(c, segment);
                }

                region = allocate_region(c, alloc_index, segment);
                if (region == nullptr)
                    return nullptr;
                activate_region(c, region);
                add_region_to_active(c, region, alloc_config);
            }

            if (region_is_block_based(c, region))
            {
                // TODO, allocate a block from a region and make sure enough pages are committed
                return nullptr;
            }

            // allocate from chunk-based region
            chunk_t* chunk = get_active_chunk(c, region);
            if (chunk == nullptr)
            {
                chunk = allocate_chunk_from_region(c, region);
                if (chunk == nullptr)
                    return nullptr;
                activate_chunk(c, region, chunk);
                add_chunk_to_active(c, region, chunk);

                // TODO does the region still have empty chunks ?
            }

            byte* chunk_address = get_region_chunk_address(c, region, region_chunk_index(c, region, chunk));
            void* ptr           = alloc_from_chunk(c, chunk, chunk_address, alloc_config.m_alloc_size);
            if (chunk_is_full(chunk))
            {
                remove_chunk_from_active(c, region, chunk);
            }
            return ptr;
        }

        void dealloc(calloc_t* c, void* ptr)
        {
            const u32   segment_index   = (u32)(((const byte*)ptr - c->m_allocations_base) >> c->m_segment_size_shift);
            const byte* segment_address = c->m_allocations_base + (segment_index << c->m_segment_size_shift);

            const u8              region_size_shift = c->m_segments[segment_index].m_region_size_shift;
            const u32             region_index      = (u32)(((const byte*)ptr - segment_address) >> region_size_shift);
            region_t*             region            = &c->m_regions[(segment_index * 32) + region_index];
            const byte*           region_address    = c->m_regions_base + ((u64)segment_index << c->m_segment_size_shift) + ((u64)region_index << region_size_shift);
            const alloc_config_t& alloc_config      = c->m_alloc_configs[region->m_alloc_index];
            if (alloc_config.m_block_size_shift > 0)
            {
                const u32 block_index = ((const byte*)ptr - region_address) >> alloc_config.m_block_size_shift;
                // TODO, deallocating from a block:
                // - validate address within region
                // - calculate block index
                // - mark block as free
                // - if required, decommit pages
            }
            else
            {
                const u32   chunk_index       = ((const byte*)ptr - region_address) >> alloc_config.m_chunk_size_shift;
                chunk_t*    chunk             = region_chunk(c, region, chunk_index);
                const byte* chunk_address     = region_address + (chunk_index << alloc_config.m_chunk_size_shift);
                const bool  chunk_full_before = chunk_is_full(chunk);
                if (chunk->m_free_bin1 == 0xFFFFFFFF)
                {
                    nbinmap5::clr(&chunk->m_free_bin0, chunk->m_free_index, chunk->m_count);
                }
                else
                {
                    u32* bin1 = (u32*)nfsa::idx2ptr(c->m_internal_fsa, chunk->m_free_bin1);
                    nbinmap10::clr(&chunk->m_free_bin0, bin1, chunk->m_free_index, chunk->m_count);
                }
                chunk->m_count -= 1;

                if (chunk_is_empty(chunk))
                {
                    // TODO:
                    // - need logic for caching empty chunks
                    // - remove chunk from active list
                    // - remove chunk from region
                    // - deactivate chunk
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
