#include "ccore/c_target.h"
#include "ccore/c_bitvec.h"
#include "ccore/c_statevec.h"
#include "ccore/c_debug.h"
#include "ccore/c_limits.h"
#include "ccore/c_memory.h"
#include "ccore/c_math.h"

#include "csuperalloc/c_superalloc_v3.h"

namespace ncore
{
    namespace nsuperallocv3
    {
        // Note:
        // Thought, it seems a region of 16MB with Chunks of 64KB could both use
        // chunks and blocks, nothing is preventing a region from being used in
        // both ways, right? The only thing we need is to know the type when picking
        // an item from the array in the region.
        // Each struct (chunk_t, block_t, region_t) now has the same initial layout:
        //   - m_type
        //   - m_alloc_cfg_index
        // So we can identify the type of structure (chunk, block, or region) when
        // obtaining it with an u32 offset.

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
        // Size: 20 bytes (must be <= 32 bytes)
        struct block_t
        {
            u8  m_type;             // 1, block type (e.g., fixed-size block)
            u8  m_alloc_cfg_index;  // allocation configuration index for this block
            u16 m_index;            // index into region->m_o_array
            u32 m_pages;            // number of committed pages in the block
            u32 m_o_region;         // region this block belongs to (offset into arena memory to a region_t)
            u32 m_tag;              // tag for the block (e.g., for tracking usage or type)
            u32 m_o_next;           // next block in list (offset into arena memory to a block_t)
        };

        // A region consists of N chunks/blocks
        // Example size of a region: 16 MB
        // Note: A region is locked to a specific region configuration
        // Size: 24 bytes (must be <= 32 bytes)
        struct region_t
        {
            u8  m_type;              // 2, region type (e.g., chunk-based or block-based)
            u8  m_region_cfg_index;  // region configuration index for this region
            u16 m_reserved0;         // reserved for future use
            u16 m_count;             // number of chunks/blocks in the region
            u16 m_capacity;          // capacity of chunks/blocks in the region
            u16 m_free_slot_index;   // index of the first virgin chunk/block slot in m_array
            u16 m_free_list;         // index of first slot of the free list for chunks/blocks
            u32 m_index;             // index into allocator->m_o_region_lookup_array
            u32 m_o_array;           // array of chunk/block offsets (offset into arena memory to a array_t)
            u32 m_o_next;            // next region in list (offset into arena memory)
        };

        // clang-format off
#define D_ALLOC_CONFIG(index, region_size_shift, chunk_size_shift, chunk_item_cap_shift) \
        {                                                                                \
          (index),                                                                       \
          (region_size_shift),                                                           \
          (chunk_size_shift),                                                            \
          (chunk_item_cap_shift)                                                         \
        }
        // clang-format on

#ifdef D_CSUPERALLOC_V3_D2
        // D == 2
        static alloc_config_t s_alloc_configs[] = {
          D_ALLOC_CONFIG(6, c16MiB, c16KiB, 11),    // 8, bin1, u64[32], 256 bytes
          D_ALLOC_CONFIG(7, c16MiB, c16KiB, 10),    // 12, bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(8, c16MiB, c16KiB, 10),    // 16, bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(9, c16MiB, c16KiB, 10),    // 24, bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(10, c16MiB, c32KiB, 10),   // 32, bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(11, c16MiB, c32KiB, 10),   // 48, bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(12, c16MiB, c64KiB, 10),   // 64, bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(13, c16MiB, c64KiB, 10),   // 96, bin1, u64[16], 128 bytes
          D_ALLOC_CONFIG(14, c16MiB, c64KiB, 9),    // 128, bin1, u64[8], 64 bytes
          D_ALLOC_CONFIG(15, c16MiB, c64KiB, 9),    // 192, bin1, u64[8], 64 bytes
          D_ALLOC_CONFIG(16, c16MiB, c64KiB, 8),    // 256, bin1, u64[4], 32 bytes
          D_ALLOC_CONFIG(17, c16MiB, c64KiB, 8),    // 384, bin1, u64[4], 32 bytes
          D_ALLOC_CONFIG(18, c16MiB, c64KiB, 7),    // 512, bin1, u64[2], 16 bytes
          D_ALLOC_CONFIG(19, c16MiB, c64KiB, 7),    // 768, bin1, u64[2], 16 bytes
          D_ALLOC_CONFIG(20, c16MiB, c64KiB, 6),    // 1 KiB, bin1 is unused
          D_ALLOC_CONFIG(21, c16MiB, c64KiB, 6),    // 1.5 KiB, bin1 is unused
          D_ALLOC_CONFIG(22, c16MiB, c64KiB, 5),    // 2 KiB, bin1 is unused
          D_ALLOC_CONFIG(23, c16MiB, c64KiB, 5),    // 3 KiB, bin1 is unused
          D_ALLOC_CONFIG(24, c16MiB, c64KiB, 4),    // 4 KiB, bin1 is unused
          D_ALLOC_CONFIG(25, c16MiB, c64KiB, 4),    // 6 KiB, bin1 is unused
          D_ALLOC_CONFIG(26, c16MiB, c64KiB, 3),    // 8 KiB, bin1 is unused
          D_ALLOC_CONFIG(27, c16MiB, c16KiB, 0),    // 12 KiB, block based region
          D_ALLOC_CONFIG(28, c16MiB, c16KiB, 0),    // 16 KiB, block based region
          D_ALLOC_CONFIG(29, c16MiB, c32KiB, 0),    // 24 KiB, block based region
          D_ALLOC_CONFIG(30, c16MiB, c32KiB, 0),    // 32 KiB, block based region
          D_ALLOC_CONFIG(31, c16MiB, c64KiB, 0),    // 48 KiB, block based region
          D_ALLOC_CONFIG(32, c16MiB, c64KiB, 0),    // 64 KiB, block based region
          D_ALLOC_CONFIG(33, c16MiB, c128KiB, 0),   // 96 KiB, block based region
          D_ALLOC_CONFIG(34, c16MiB, c128KiB, 0),   // 128 KiB, block based region
          D_ALLOC_CONFIG(35, c16MiB, c256KiB, 0),   // 192 KiB, block based region
          D_ALLOC_CONFIG(36, c16MiB, c256KiB, 0),   // 256 KiB, block based region
          D_ALLOC_CONFIG(37, c16MiB, c512KiB, 0),   // 384 KiB, block based region
          D_ALLOC_CONFIG(38, c16MiB, c512KiB, 0),   // 512 KiB, block based region
          D_ALLOC_CONFIG(39, c32MiB, c1MiB, 0),     // 768 KiB, block based region
          D_ALLOC_CONFIG(40, c32MiB, c1MiB, 0),     // 1 MiB, block based region
          D_ALLOC_CONFIG(41, c32MiB, c2MiB, 0),     // 1.5 MiB, block based region
          D_ALLOC_CONFIG(42, c32MiB, c2MiB, 0),     // 2 MiB, block based region
          D_ALLOC_CONFIG(43, c32MiB, c4MiB, 0),     // 3 MiB, block based region
          D_ALLOC_CONFIG(44, c32MiB, c4MiB, 0),     // 4 MiB, block based region
          D_ALLOC_CONFIG(45, c32MiB, c8MiB, 0),     // 6 MiB, block based region
          D_ALLOC_CONFIG(46, c32MiB, c8MiB, 0),     // 8 MiB, block based region
          D_ALLOC_CONFIG(47, c256MiB, c16MiB, 0),   // 12 MiB, block based region
          D_ALLOC_CONFIG(48, c256MiB, c16MiB, 0),   // 16 MiB, block based region
          D_ALLOC_CONFIG(49, c256MiB, c32MiB, 0),   // 24 MiB, block based region
          D_ALLOC_CONFIG(50, c256MiB, c32MiB, 0),   // 32 MiB, block based region
          D_ALLOC_CONFIG(51, c256MiB, c64MiB, 0),   // 48 MiB, block based region
          D_ALLOC_CONFIG(52, c256MiB, c64MiB, 0),   // 64 MiB, block based region
          D_ALLOC_CONFIG(53, c256MiB, c128MiB, 0),  // 96 MiB, block based region
          D_ALLOC_CONFIG(54, c256MiB, c128MiB, 0),  // 128 MiB, block based region
          D_ALLOC_CONFIG(55, c512MiB, c256MiB, 0),  // 192 MiB, block based region
          D_ALLOC_CONFIG(56, c512MiB, c256MiB, 0),  // 256 MiB, block based region
          D_ALLOC_CONFIG(57, c1GiB, c512MiB, 0),    // 384 MiB, block based region
          D_ALLOC_CONFIG(58, c1GiB, c512MiB, 0),    // 512 MiB, block based region
        };

        static const s32 cAllocConfigCount = DARRAYSIZE(s_alloc_configs);
        static const s32 cMinBin           = 6;
        static const s32 cMaxBin           = cMinBin + cAllocConfigCount - 1;
#endif

        static const u32 cConfigSlots       = 256;
        static const u32 cMaxConfigs        = 255;
        static const u32 cShiftValues       = 32;
        static const u32 cPresenceBytes     = cShiftValues * cShiftValues;
        static const u32 cConfigArraysBytes = 4 * cConfigSlots * 2;

        bool s_build_chunk_configs(const alloc_config_t* configs, u32 count, chunk_config_t* chunks, u8* presence, u16& chunk_count)
        {
            nmem::memset(presence, 0, cPresenceBytes);

            chunk_count = 0;
            for (u32 i = 0; i < count; ++i)
            {
                const alloc_config_t& config = configs[i];
                if (config.m_chunk_size_shift >= cShiftValues || config.m_item_cap_shift >= cShiftValues)
                    return false;
                u8& present = presence[config.m_chunk_size_shift * cShiftValues + config.m_item_cap_shift];
                if (present == 0)
                {
                    if (chunk_count == cMaxConfigs)
                        return false;
                    present             = 1;
                    chunks[chunk_count] = {config.m_chunk_size_shift, config.m_item_cap_shift};
                    ++chunk_count;
                }
            }
            return true;
        }

        bool s_build_region_configs(const alloc_config_t* configs, u32 count, region_config_t* regions, u8* presence, u16& region_count)
        {
            nmem::memset(presence, 0, cPresenceBytes);

            region_count = 0;
            for (u32 i = 0; i < count; ++i)
            {
                const alloc_config_t& config = configs[i];
                if (config.m_region_size_shift >= cShiftValues)
                    return false;
                u8& present = presence[config.m_region_size_shift * cShiftValues + config.m_chunk_size_shift];
                if (present == 0)
                {
                    if (region_count == cMaxConfigs)
                        return false;
                    present               = 1;
                    regions[region_count] = {config.m_region_size_shift, config.m_chunk_size_shift};
                    ++region_count;
                }
            }
            return true;
        }

        void* s_allocate(arena_pool_t* arena)
        {
            if (arena->m_free_list == 0xffffffff)
            {
                const u64 used     = (u64)(arena->m_memory_cursor - arena->m_memory_base);
                const u64 end      = used + arena->m_allocation_size;
                const u64 reserved = (u64)arena->m_reserved_pages << arena->m_page_sizeshift;
                if (end > reserved)
                    return nullptr;

                const u64 page_size      = (u64)1 << arena->m_page_sizeshift;
                const u32 required_pages = (u32)((end + page_size - 1) >> arena->m_page_sizeshift);
                if (required_pages > arena->m_committed_pages)
                {
                    byte*     commit_base = arena->m_memory_base + ((u64)arena->m_committed_pages << arena->m_page_sizeshift);
                    const u64 commit_size = (u64)(required_pages - arena->m_committed_pages) << arena->m_page_sizeshift;
                    if (!v_alloc_commit(commit_base, (uint_t)commit_size))
                        return nullptr;
                    arena->m_committed_pages = required_pages;
                }

                void* ptr = arena->m_memory_cursor;
                arena->m_memory_cursor += arena->m_allocation_size;
                return ptr;
            }
            else
            {
                void* ptr          = arena->m_memory_base + arena->m_free_list;
                arena->m_free_list = *((u32*)ptr);
                return ptr;
            }
        }

        void s_free(arena_pool_t* arena, void* ptr)
        {
            ASSERT(((uint_t)ptr & 0x7) == 0);  // Ensure the pointer is 8-byte aligned
            *((u32*)ptr)       = arena->m_free_list;
            arena->m_free_list = (u32)((byte*)ptr - arena->m_memory_base);
        }

        bool shutdown(allocator_t* a)
        {
            if (a == nullptr)
                return false;

            bool success = true;
            if (a->m_memory_base != nullptr)
            {
                if (v_alloc_release(a->m_memory_base, (uint_t)a->m_memory_size))
                    a->m_memory_base = nullptr;
                else
                    success = false;
            }
            if (a->m_arena_memory_base != nullptr)
            {
                if (v_alloc_release(a->m_arena_memory_base, (uint_t)a->m_arena_memory_size))
                    a->m_arena_memory_base = nullptr;
                else
                    success = false;
            }
            if (a->m_config_memory_base != nullptr)
            {
                if (v_alloc_release(a->m_config_memory_base, (uint_t)a->m_config_memory_size))
                    a->m_config_memory_base = nullptr;
                else
                    success = false;
            }
            if (success)
                *a = allocator_t{};
            return success;
        }

        bool initialize(allocator_t* a, u64 managed_size, u32 pool_size)
        {
            if (a == nullptr || a->m_memory_base != nullptr || a->m_arena_memory_base != nullptr || a->m_config_memory_base != nullptr)
                return false;

            const u32 page_size       = v_alloc_get_page_size();
            const u8  page_shift      = v_alloc_get_page_size_shift();
            const u64 min_region_size = (u64)1 << c16MiB;
            const u64 pool_total      = (u64)pool_size * 12;
            if (page_size == 0 || (page_size & (page_size - 1)) != 0 || page_size > ((u32)1 << c16KiB) || page_shift >= 32 || ((u32)1 << page_shift) != page_size || managed_size < min_region_size || (managed_size & (min_region_size - 1)) != 0 ||
                (u64)(uint_t)managed_size != managed_size || pool_size < 8192 || pool_size < page_size || (pool_size & (pool_size - 1)) != 0 || pool_total > 0xffffffffu || (u64)(uint_t)pool_total != pool_total)
                return false;

            const u64 lookup_count = managed_size >> c16MiB;
            if (lookup_count > 0xffffffffu / sizeof(u32))
                return false;

            const u64 max_config_bytes     = cConfigArraysBytes + (u64)cAllocConfigCount * sizeof(alloc_config_t) + ((u64)cMaxBin + 1 + cMaxConfigs * 3 + lookup_count) * sizeof(u32);
            const u64 scratch_end          = cConfigArraysBytes + cPresenceBytes;
            const u64 required_reservation = max_config_bytes > scratch_end ? max_config_bytes : scratch_end;
            const u64 config_size          = (required_reservation + page_size - 1) & ~((u64)page_size - 1);
            const u64 initial_commit       = (scratch_end + page_size - 1) & ~((u64)page_size - 1);
            if (config_size > 0xffffffffu || (u64)(uint_t)config_size != config_size)
                return false;

            a->m_memory_size = managed_size;
            a->m_memory_base = (byte*)v_alloc_reserve((uint_t)managed_size);
            if (a->m_memory_base == nullptr)
            {
                *a = allocator_t{};
                return false;
            }
            a->m_arena_memory_size = pool_total;
            a->m_arena_memory_base = (byte*)v_alloc_reserve((uint_t)pool_total);
            if (a->m_arena_memory_base == nullptr)
            {
                if (!shutdown(a))
                    ASSERTS(false, "Failed to release v3 allocator reservations");
                return false;
            }
            a->m_config_memory_size = config_size;
            a->m_config_memory_base = (byte*)v_alloc_reserve((uint_t)config_size);
            if (a->m_config_memory_base == nullptr || !v_alloc_commit(a->m_config_memory_base, (uint_t)initial_commit))
            {
                if (!shutdown(a))
                    ASSERTS(false, "Failed to release v3 allocator reservations");
                return false;
            }

            a->m_chunk_config_array  = (chunk_config_t*)a->m_config_memory_base;
            a->m_region_config_array = (region_config_t*)(a->m_config_memory_base + cConfigSlots * sizeof(chunk_config_t));
            u8* presence             = a->m_config_memory_base + cConfigArraysBytes;
            u16 chunk_count          = 0;
            u16 region_count         = 0;
            if (!s_build_configs(s_alloc_configs, cAllocConfigCount, a->m_chunk_config_array, a->m_region_config_array, presence, chunk_count, region_count))
            {
                if (!shutdown(a))
                    ASSERTS(false, "Failed to release v3 allocator reservations");
                return false;
            }

            const u64 config_bytes = cConfigArraysBytes + (u64)cAllocConfigCount * sizeof(alloc_config_t) + ((u64)cMaxBin + 1 + chunk_count + 2 * region_count + lookup_count) * sizeof(u32);
            const u64 final_commit = (config_bytes + page_size - 1) & ~((u64)page_size - 1);
            if (final_commit > initial_commit && !v_alloc_commit(a->m_config_memory_base + initial_commit, (uint_t)(final_commit - initial_commit)))
            {
                if (!shutdown(a))
                    ASSERTS(false, "Failed to release v3 allocator reservations");
                return false;
            }

            a->m_num_alloc_configs    = (u16)cAllocConfigCount;
            a->m_num_chunk_configs    = chunk_count;
            a->m_num_region_configs   = region_count;
            a->m_min_region_sizeshift = c16MiB;
            u8 pool_shift             = 0;
            for (u32 size = pool_size; size > 1; size >>= 1)
                ++pool_shift;
            a->m_arena_size_sizeshift = (s8)pool_shift;

            byte* cursor            = a->m_config_memory_base + cConfigArraysBytes;
            a->m_alloc_config_array = (alloc_config_t*)cursor;
            cursor += cAllocConfigCount * sizeof(alloc_config_t);
            a->m_not_full_o_chunks_per_alloc_index = (u32*)cursor;
            cursor += (cMaxBin + 1) * sizeof(u32);
            a->m_cached_empty_o_chunks_per_chunk_index = (u32*)cursor;
            cursor += chunk_count * sizeof(u32);
            a->m_active_o_region_per_region_index = (u32*)cursor;
            cursor += region_count * sizeof(u32);
            a->m_unused_o_region_per_region_index = (u32*)cursor;
            cursor += region_count * sizeof(u32);
            a->m_o_region_lookup_array = (u32*)cursor;

            for (s32 i = 0; i < cAllocConfigCount; ++i)
                a->m_alloc_config_array[i] = s_alloc_configs[i];
            for (s32 i = 0; i <= cMaxBin; ++i)
                a->m_not_full_o_chunks_per_alloc_index[i] = 0xffffffffu;
            for (u16 i = 0; i < chunk_count; ++i)
            {
                a->m_cached_empty_o_chunks_per_chunk_index[i] = 0xffffffffu;
            }
            for (u16 i = 0; i < region_count; ++i)
            {
                a->m_active_o_region_per_region_index[i] = 0xffffffffu;
                a->m_unused_o_region_per_region_index[i] = 0xffffffffu;
            }
            for (u64 i = 0; i < lookup_count; ++i)
                a->m_o_region_lookup_array[i] = 0xffffffffu;

            for (u32 i = 0; i < 12; ++i)
            {
                arena_pool_t& pool     = a->m_arenas[i];
                pool.m_reserved        = 0;
                pool.m_page_sizeshift  = page_shift;
                pool.m_allocation_size = (u16)(i == 0 ? 32 : (u32)1 << (i + 2));
                pool.m_free_list       = 0xffffffffu;
                pool.m_committed_pages = 0;
                pool.m_reserved_pages  = pool_size >> page_shift;
                pool.m_memory_base     = a->m_arena_memory_base + (u64)i * pool_size;
                pool.m_memory_cursor   = pool.m_memory_base;
            }
            return true;
        }

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
            ASSERT(size >= 8 && size <= 8192);

            // 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
            // TODO optimize this using bit manipulation (e.g., find the index using log2(size) - 3)

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
            alloc_config_t* config     = &s_alloc_configs[bin - cMinBin];
            const u32       class_size = ((u32)1 << (bin / 2)) + ((bin & 1) ? ((u32)1 << ((bin / 2) - 1)) : 0);
            ASSERT(alloc_size <= class_size);
            return config;
        }

        void* allocate(allocator_t* a, u32 size)
        {
            // - alloc_config_t* ac = get_alloc_config_for_size(a, size);
            // - Is there an active chunk for this allocation config
            //   - If not, check if there is an active region for this allocation config
            //     - If not, allocate and initialize a new region for the allocation configuration
            //       - Add the new region to the active region list
            //     - If yes, allocate and initialize a new chunk within the active region
            //       - Add the chunk to the active chunk list
            // - Allocate item from the active chunk
            // - If chunk is now full remove it from the active list
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
            //   -> Free the chunk structure.
            //      Add the region chunk array slot to the free-list in the region
            //   -> Or if we have a cache we can add the chunk to the cache list.
            // - If the chunk was full before this deallocation, add it back to the active list
        }

    }  // namespace nsuperallocv3

}  // namespace ncore
