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
        // N region structs exist in a continues separate address space with the following size:
        // Number-Of-Regions * N MiB. So each region has N MiB of address space to use for
        // its own management.
        // Each region starts at X * N MiB, where X is the region index (0,1,2,3,...)
        // The amount of committed pages for a region depends on the configuration of that region.
        // Example:
        // Region-Size in the main address space = 2 GiB
        // Chunk-Size = 64 KiB
        // Number of chunks for region to manage = 2 GiB / 64 KiB = 32768 chunks
        // sizeof(chunk_t) = 32 bytes * 32768 = 1.0 MiB for chunk array
        // However we will not fully commit the full chunk array, but will commit page by page on demand.
        // On MacOS, page-size = 16 KiB, so with one page we can already track 16 KiB / 32 B = 512 chunks.
        // The first committed page starts with struct region_t and followed by the chunk or block array.
        // sizeof(region_t) = 64 bytes
        //
        // When a region is using blocks, it is obvious that there will not be many block and so we will not
        // even use most of the first committed page.
        // We will accept this 'waste' for the benefit of simplicity, also there are not many regions to
        // begin with and even less that are 'block' based. In a large address space of 256 GiB we have
        // 256GiB/2GiB = 128 regions.
        //
        // The address range of a region can also be set to 1GiB, this will increase the number of regions
        // accross the address space, but will reduce the number of chunks/blocks per region. Or it will
        // enable to have smaller chunk sizes while keeping the number of chunks/blocks per region manageable.
        //
        // Region-Size in the main address space = 1GiB
        // Chunk-Size = 16 KiB
        // Number of chunks for region to manage = 1 GiB / 16 KiB = 65536 chunks
        // sizeof(chunk_t) = 32 bytes * 65536 = 2.0 MiB for chunk array
        //
        // Chunk-Size = 64 KiB
        // Number of chunks for region to manage = 1 GiB / 64 KiB = 16384 chunks
        // sizeof(chunk_t) = 32 bytes * 16384 = 0.5 MiB for chunk array
        //
        // With a full address range of 256 GiB we would have 256 regions of 1 GiB each.
        // This would mean a maximum of 256 * 2.0 MiB = 512 MiB of address space used for chunk arrays
        //
        // Note: If we also want to have the meta-data for chunks be part of the region data then we need
        // max binmap bin1 = 1024 elements -> 1024 bits = 128 bytes per chunk
        // max number of chunks = 65536
        // 128 bytes * 65536 = 8 MiB (bin1) + 2 MiB (chunk_t array) = 10 MiB per region (we could limit it to 32 MiB?)
        // With 256 regions this would be 256 * 34 MiB = 8.5 GiB of address space used for chunk meta-data
        //
        // The above setup makes the whole management of regions and chunks/blocks a lot simpler.
        //
        // Note: The array of sections will be outside of the region address space, so that we have
        //       all the 2 MiB available for either the array of chunks or the array of blocks.

        /*
        Blocks:

        To manage blocks, every region has a block array (block_t[]) instead of chunk array (chunk_t[]). The sizeof(block_t)
        is 8 bytes, and in 2 MiB we can hold 2 MiB / 8 = 256 K blocks.

        A region is 4 GiB, so the smallest block we can possibly manage is 4 GiB / 256 K = 16 KiB.

        Blocks are handled differently than chunks, as blocks are committed page by page. So when a block is requested,
        we need to make sure that the block has enough committed pages to fullfill the request. If not, we need to commit
        (or decommit) pages to match the requested allocation size.

        */

        /*
        Caching:

          Every bin (should be) configured with some details on how many chunks/block it should cache.
          When a chunk/block is freed, it is added to the bin's cache list. When the cache list exceeds the
          maximum number of cached chunks/blocks, the oldest chunk/block is removed from the cache, deactivated
          and added to the free list.

          OR

          Should we just track the amount of 'memory' that is cached, and when exceeding a certain threshold,
          we remove oldest chunks/blocks until we are below the threshold again?

        */

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

        struct bin_config_t
        {
            u32 m_alloc_size;        // size of elements in this bin
            u8  m_chunk_size_shift;  // chunk size shift for chunks in this bin
            u8  m_region_type;       // region type is chunk or block (0=chunks, >=1 = block size shift)
        };

#define D_MAX_ELEMENTS_PER_CHUNK 1024

        // A region consists of N chunks
        // A chunk consists of N elements with a maximum of 1024.
        // = 24 bytes (16384 / 24 = 682 chunks in one page)
        struct chunk_t
        {
            u16  m_pages;       // number of committed pages in the chunk
            u16  m_capacity;    // number of elements in the chunk
            u16  m_count;       // number of elements in the chunk
            u16  m_free_index;  // current element free index
            u64  m_free_bin0;   // binmap, level 0 (6)
            u64* m_free_bin1;   // binmap, level 1 (6) (fsa)
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

#define D_MAX_CHUNKS_PER_REGION 512
#define D_MAX_BLOCKS_PER_REGION 512

        // A region consists of N chunks/blocks
        // - maximum 512 chunks
        // - maximum 512 blocks
        // Chunks/Blocks in a region are of the same size (chunk_size_shift).
        // A region is dedicated to a specific bin (allocation size).
        struct region_t
        {
            u8   m_bin;                // bin index for this region
            u8   m_type;               // region type (0=chunks, >=1 = block size shift)
            u16  m_free_index;         // index of the first free chunk/block in the region
            u16  m_count;              // number of chunks/blocks in use
            u16  m_capacity;           // total number of chunks/blocks in this region
            u32  m_chunk_free_bin0;    // free chunks binmap, level 0
            u32* m_chunk_free_bin1;    // free chunks binmap, level 1 (fsa)
            u32  m_chunk_active_bin0;  // active chunks binmap, level 0
            u32* m_chunk_active_bin1;  // active chunks binmap, level 1 (fsa)
        };

        inline bool chunk_is_full(chunk_t* chunk) { return chunk->m_count == chunk->m_capacity; }
        inline bool chunk_is_empty(chunk_t* chunk) { return chunk->m_count == 0; }
        inline u32  region_chunk_index(region_t* region, chunk_t* chunk) { return (u32)(((byte*)chunk - (byte*)region->m_chunks.m_array) / sizeof(chunk_t)); }
        inline bool region_is_block_based(region_t* region) { return region->m_region_type == 1; }

        static void* alloc_from_chunk(chunk_t* chunk, byte* chunk_address, u32 alloc_size)
        {
            ASSERT(chunk->m_count < chunk->m_capacity);
            i32 free_index;
            if (chunk->m_bin1 == nullptr)
                free_index = nbinmap6::find_and_set(&chunk->m_bin0, chunk->m_count);
            else
                free_index = nbinmap12::find_and_set(&chunk->m_bin0, chunk->m_bin1, chunk->m_count);
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
        static const region_config_t sRegionConfigs[] = {
          region_config_t{c32MiB, c64KiB, 0},    // 32 MiB region, 64 KiB chunks
          region_config_t{c32MiB, 0, c64KiB},    // 32 MiB region, 64 KiB blocks
          region_config_t{c32MiB, 0, c128KiB},   // 32 MiB region, 128 KiB blocks
          region_config_t{c32MiB, 0, c256KiB},   // 32 MiB region, 256 KiB blocks
          region_config_t{c32MiB, 0, c512KiB},   // 32 MiB region, 512 KiB blocks
          region_config_t{c32MiB, 0, c1MiB},     // 32 MiB region, 1 MiB blocks
          region_config_t{c32MiB, 0, c2MiB},     // 32 MiB region, 2 MiB blocks
          region_config_t{c32MiB, 0, c4MiB},     // 32 MiB region, 4 MiB blocks
          region_config_t{c32MiB, 0, c8MiB},     // 32 MiB region, 8 MiB blocks
          region_config_t{c256MiB, 0, c16MiB},   // 256 MiB region, 16 MiB blocks
          region_config_t{c256MiB, 0, c32MiB},   // 256 MiB region, 32 MiB blocks
          region_config_t{c256MiB, 0, c64MiB},   // 256 MiB region, 64 MiB blocks
          region_config_t{c256MiB, 0, c128MiB},  // 256 MiB region, 128 MiB blocks
          region_config_t{c512MiB, 0, c256MiB},  // 512 MiB region, 256 MiB blocks
          region_config_t{c1GiB, 0, c512MiB},    // 1 GiB region, 512 MiB blocks
        };

        static const bin_config_t sBinConfigs[] = {
          bin_config_t{(1 << 4) + (1 << (4 - 3)) * 1},
        };

        struct segment_t
        {
            u32 m_free_bin0;  // binmap level 0 of free regions
            u8  m_count;      // number of regions
        };

        struct calloc_t
        {
            byte*          m_address_base;            // base address of the superallocator
            int_t          m_address_size;            // size of the address space
            arena_t*       m_internal_heap;           // internal heap for initialization allocations
            fsa_t*         m_internal_fsa;            // internal fsa for runtime allocations
            u8             m_region_size_shift;       // size of each section (e.g. 2 GiB)
            u8             m_page_size_shift;         // system page size
            byte*          m_region_meta_base;        // base address of region metadata address space (N * 2MiB)
            u8             m_region_meta_size_shift;  // size of each region structure in 'regions_base' (e.g. 2 MiB)
            u32            m_region_meta_count;       // number of address sections in the whole address space
            u64            m_region_meta_free_bin0;   // binmap bin0 of free address sections
            u64*           m_region_meta_free_bin1;   // binmap bin1 of free address sections (heap)
            size_to_bin_fn m_size_to_bin;             // function to map size to bin index
            bin_config_t*  m_bin_configs;             // bin configurations
            region_t**     m_active_region_per_bin;   // active region per bin
            region_t*      m_regions;                 // all regions
            segment_t*     m_segments;
        };

        static inline region_t* get_region_at_index(calloc_t* c, u32 region_index) { return &c->m_regions[region_index]; }
        static inline u32       get_region_index(calloc_t* c, region_t* region) { return (u32)(region - c->m_regions); }
        static inline byte*     get_region_address(calloc_t* c, u32 region_index) { return c->m_address_base + (region_index << (c->m_region_size_shift + c->m_page_size_shift)); }
        static inline byte*     get_region_chunk_address(calloc_t* c, region_t* region, u32 chunk_index)
        {
            const u32 region_index   = get_region_index(c, region);
            byte*     region_address = get_region_address(c, region_index);
            return region_address + ((int_t)chunk_index << c->m_bin_configs[region->m_bin].m_chunk_size_shift);
        }

        region_t* get_region(calloc_t* c, u8 bin_index)
        {
            const bin_config_t& bin_config = c->m_bin_configs[bin_index];
            {
                // see if we have an active region for this config
                // TODO this is actually a list (region should get next/prev pointers)
                region_t* active_region = c->m_active_region_per_bin[bin_index];
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
                    region->m_bin        = bin_index;
                    region->m_type       = bin_config.m_region_type;
                    region->m_free_index = 0;

                    // initialize region
                    if (bin_config.m_region_type == D_USAGE_CHUNK)
                    {
                        v_alloc_commit(region_address, (int_t)1 << c->m_page_size_shift);  // commit first page
                        region->m_region_committed_pages = 1;
                        region->m_free_index_threshold   = (((int_t)1 << c->m_page_size_shift) - sizeof(region_t)) / sizeof(chunk_t);
                    }
                    c->m_active_region_per_chunk_size[bin_config.m_chunk_size_shift] = region;
                    return region;
                }

                return nullptr;
            }

            void release_region(calloc_t * c, region_t * region)
            {
                ASSERT(region->m_count == 0);
                const u32 region_index = get_region_index(c, region);
                nbinmap12::clear(&c->m_region_meta_free_bin0, c->m_region_meta_free_bin1, region_index);
                v_alloc_decommit(get_region_address(c, region_index), region->m_region_committed_pages << c->m_page_size_shift);
            }

            chunk_t* get_chunk_from_region(calloc_t * c, region_t * region)
            {
                chunk_t* chunk = nullptr;
                if (region->m_chunks.m_free_list != nullptr)
                {
                    chunk                        = region->m_chunks.m_free_list;
                    region->m_chunks.m_free_list = &region->m_chunks.m_array[chunk->m_next];
                }
                else
                {
                    if (region->m_free_index == region->m_free_index_threshold)
                    {
                        if (region->m_region_committed_pages == region->m_region_maximum_pages)
                            return nullptr;
                        byte* region_address = get_region_address(c, get_region_index(c, region));
                        v_alloc_commit(region_address + (region->m_region_committed_pages << c->m_page_size_shift), (int_t)1 << c->m_page_size_shift);
                        region->m_region_committed_pages++;
                        region->m_free_index_threshold = ((region->m_region_committed_pages << c->m_page_size_shift) - sizeof(region_t)) / sizeof(chunk_t);
                    }
                    chunk = &region->m_chunks.m_array[region->m_free_index];
                    region->m_free_index++;
                }

                chunk->m_prev = D_NILL_U16;
                chunk->m_next = D_NILL_U16;

                return chunk;
            }

            static inline void activate_chunk(calloc_t * c, region_t * region, chunk_t * chunk, u8 bin_index)
            {
                const bin_config_t& bincfg = c->m_bin_configs[bin_index];

                // initialize chunk
                chunk->m_bin_index  = bin_index;
                chunk->m_capacity   = (1 << bincfg.m_chunk_size_shift) / bincfg.m_alloc_size;
                chunk->m_count      = 0;
                chunk->m_free_index = 0;
                chunk->m_pages      = ((int_t)1 << (bincfg.m_chunk_size_shift - c->m_page_size_shift));
                chunk->m_bin0       = D_U64_MAX << ((chunk->m_capacity + 63) >> 6);
                if (chunk->m_capacity > 64)
                {
                    chunk->m_bin1 = (u64*)nfsa::allocate(c->m_internal_fsa, ((chunk->m_capacity + 63) >> 6) * sizeof(u64));
                }
                else
                {
                    chunk->m_bin1 = nullptr;
                }

                // TODO commit all pages for this chunk
                void* chunk_address = get_region_chunk_address(c, region, region_chunk_index(region, chunk));
                v_alloc_commit(chunk_address, chunk->m_pages << c->m_page_size_shift);
            }

            static inline void deactivate_chunk(calloc_t * c, chunk_t * chunk)
            {
                // TODO decommit all pages

                chunk->m_bin_index  = D_NILL_U8;
                chunk->m_capacity   = 0;
                chunk->m_count      = 0;
                chunk->m_free_index = 0;
                chunk->m_pages      = 0;
                chunk->m_bin0       = 0;
                if (chunk->m_bin1 != nullptr)
                {
                    nfsa::deallocate(c->m_internal_fsa, chunk->m_bin1);
                    chunk->m_bin1 = nullptr;
                }
            }

            void release_chunk_to_region(calloc_t * c, region_t * region, chunk_t * chunk)
            {
                chunk_t* head = region->m_chunks.m_free_list;
                if (head == nullptr)
                {
                    chunk->m_prev = D_NILL_U16;
                    chunk->m_next = D_NILL_U16;
                }
                else
                {
                    chunk->m_prev = D_NILL_U16;
                    chunk->m_next = (u16)(region_chunk_index(region, head));
                    head->m_prev  = (u16)(region_chunk_index(region, chunk));
                }
                region->m_chunks.m_free_list = chunk;
            }

            chunk_t* get_active_chunk(calloc_t * c, u8 bin_index)
            {
                chunk_t* active_chunk = c->m_active_chunk_per_bin_config[bin_index];
                return active_chunk;
            }

            void add_chunk_to_active_list(calloc_t * c, u8 bin_index, chunk_t * chunk)
            {
                ASSERT(chunk->m_prev == D_NILL_U16 && chunk->m_next == D_NILL_U16);
                if (c->m_active_chunk_per_bin_config[bin_index] == nullptr)
                {
                    c->m_active_chunk_per_bin_config[bin_index] = chunk;
                }
                else
                {
                    chunk_t* head                               = c->m_active_chunk_per_bin_config[bin_index];
                    chunk->m_next                               = (u16)(region_chunk_index(nullptr, head));  // region is not needed here
                    head->m_prev                                = (u16)(region_chunk_index(nullptr, chunk));
                    c->m_active_chunk_per_bin_config[bin_index] = chunk;
                }
            }

            void remove_chunk_from_active_list(calloc_t * c, u8 bin_index, chunk_t * chunk)
            {
                chunk_t*& head = c->m_active_chunk_per_bin_config[bin_index];
                if (chunk->m_prev == D_NILL_U16 && chunk->m_next == D_NILL_U16)
                {
                    head = nullptr;
                }
                else if (chunk->m_prev == D_NILL_U16)
                {
                    region_t* region = get_region_at_index(c, chunk->m_region_index);
                    chunk_t*  next   = &region->m_chunks.m_array[chunk->m_next];
                    next->m_prev     = D_NILL_U16;
                    head             = next;
                }
                else if (chunk->m_next == D_NILL_U16)
                {
                    region_t* region = get_region_at_index(c, chunk->m_region_index);
                    chunk_t*  prev   = &region->m_chunks.m_array[chunk->m_prev];
                    prev->m_next     = D_NILL_U16;
                }
                else
                {
                    region_t* region = get_region_at_index(c, chunk->m_region_index);
                    chunk_t*  prev   = &region->m_chunks.m_array[chunk->m_prev];
                    chunk_t*  next   = &region->m_chunks.m_array[chunk->m_next];
                    prev->m_next     = chunk->m_next;
                    next->m_prev     = chunk->m_prev;
                }
                chunk->m_prev = D_NILL_U16;
                chunk->m_next = D_NILL_U16;
            }

            // Allocate memory of given size
            void* alloc(calloc_t * c, u32 size)
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
                chunk_t* chunk = get_active_chunk(c, bin_index);
                if (chunk == nullptr)
                {
                    chunk = get_chunk_from_region(c, region);
                    if (chunk == nullptr)
                        return nullptr;
                    activate_chunk(c, region, chunk, bin_index);
                    add_chunk_to_active_list(c, bin_index, chunk);
                }

                byte* chunk_address = get_region_chunk_address(c, region, region_chunk_index(region, chunk));
                void* ptr           = alloc_from_chunk(chunk, chunk_address, bin_config.m_alloc_size);
                if (chunk_is_full(chunk))
                {
                    remove_chunk_from_active_list(c, bin_index, chunk);
                }
                return ptr;
            }

            void dealloc(calloc_t * c, void* ptr)
            {
                const u32 region_index = (u32)(((const byte*)ptr - c->m_address_base) >> c->m_region_size_shift);
                ASSERTS(region_index >= 0 && region_index < c->m_region_meta_count, "invalid region index");
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
                    const u32 chunk_index = ((const byte*)ptr - region_address) >> region->m_chunk_size_shift;
                    chunk_t*  chunk       = &region->m_chunks.m_array[chunk_index];
                    // const byte* chunk_address     = region_address + (chunk_index << region->m_chunk_size_shift);
                    const bool chunk_full_before = chunk_is_full(chunk);
                    if (chunk->m_bin1 == nullptr)
                    {
                        nbinmap6::clr(&chunk->m_bin0, chunk->m_free_index, chunk->m_count);
                    }
                    else
                    {
                        nbinmap12::clr(&chunk->m_bin0, chunk->m_bin1, chunk->m_free_index, chunk->m_count);
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
