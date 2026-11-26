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

        static const u32 cConfigSlots         = 256;
        static const u32 cMaxConfigs          = 255;
        static const u32 cShiftValues         = 32;
        static const u32 cMaxRegionSlotsShift = 11;
        static const u32 cPresenceBytes       = cShiftValues * cShiftValues;
        static const u32 cConfigArraysBytes   = cConfigSlots * (sizeof(chunk_config_t) + sizeof(region_config_t) + sizeof(bin_config_t));
        CC_STATIC_ASSERT(sizeof(allocator_t) % sizeof(u32) == 0);
        CC_STATIC_ASSERT(sizeof(chunk_config_t) == 2);
        CC_STATIC_ASSERT(sizeof(region_config_t) == 2);
        CC_STATIC_ASSERT(sizeof(bin_config_t) == 4);
        CC_STATIC_ASSERT(sizeof(region_t) <= 32);
        CC_STATIC_ASSERT(sizeof(chunk_t) <= 32);
        CC_STATIC_ASSERT(sizeof(block_t) <= 32);

        bool s_build_chunk_configs(const alloc_config_t* configs, u32 count, chunk_config_t* chunks, u8* presence, u8& chunk_count)
        {
            for (u32 i = 0; i < cPresenceBytes; ++i)
                presence[i] = 0xFF;

            chunk_count = 0;
            for (u32 i = 0; i < count; ++i)
            {
                const alloc_config_t& config = configs[i];
                if (config.m_chunk_size_shift >= cShiftValues || config.m_item_cap_shift >= cShiftValues)
                    return false;
                u8& present = presence[config.m_chunk_size_shift * cShiftValues + config.m_item_cap_shift];
                if (present == 0xFF)
                {
                    if (chunk_count == cMaxConfigs)
                        return false;
                    present             = chunk_count;
                    chunks[chunk_count] = {config.m_chunk_size_shift, config.m_item_cap_shift};
                    ++chunk_count;
                }
            }
            return true;
        }

        void s_build_bin_config_chunks(const alloc_config_t* configs, u32 count, bin_config_t* bins, u8* presence)
        {
            for (u32 i = 0; i < count; ++i)
            {
                // The presence array is used to obtain the index of the chunk configuration corresponding to this bin.
                const alloc_config_t& config          = configs[i];
                const u8              chunk_cfg_index = presence[config.m_chunk_size_shift * cShiftValues + config.m_item_cap_shift];
                bins[i].m_chunk_cfg_index             = chunk_cfg_index;
            }
        }

        bool s_build_region_configs(const alloc_config_t* configs, u32 count, region_config_t* regions, u8* presence, u8& region_count)
        {
            for (u32 i = 0; i < cPresenceBytes; ++i)
                presence[i] = 0xFF;

            region_count = 0;
            for (u32 i = 0; i < count; ++i)
            {
                const alloc_config_t& config = configs[i];
                if (config.m_region_size_shift >= cShiftValues || config.m_chunk_size_shift >= cShiftValues)
                    return false;
                u8& present = presence[config.m_region_size_shift * cShiftValues + config.m_chunk_size_shift];
                if (present == 0xFF)
                {
                    if (region_count == cMaxConfigs)
                        return false;
                    present               = region_count;
                    regions[region_count] = {config.m_region_size_shift, config.m_chunk_size_shift};
                    ++region_count;
                }
            }
            return true;
        }

        static void s_build_bin_config_regions(const alloc_config_t* configs, u32 count, bin_config_t* bins, u8* presence)
        {
            for (u32 i = 0; i < count; ++i)
            {
                // The presence array is used to obtain the index of the region configuration corresponding to this bin.
                const alloc_config_t& config           = configs[i];
                const u8              region_cfg_index = presence[config.m_region_size_shift * cShiftValues + config.m_chunk_size_shift];
                bins[i].m_region_cfg_index             = region_cfg_index;
            }
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
            if (!success)
                return false;
            return v_alloc_release(a->m_config_memory_base, (uint_t)a->m_config_memory_size);
        }

        static void s_release_reservation(byte* base, u64 size)
        {
            if (base != nullptr && !v_alloc_release(base, (uint_t)size))
                ASSERTS(false, "Failed to release v3 allocator reservation");
        }

        template <typename T>
        static T* s_advance_cursor(byte*& cursor, u32 count)
        {
            T* result = (T*)cursor;
            cursor += sizeof(T) * count;
            return result;
        }

        static u64 s_item_size_for_bin(u32 index, u32 divisor)
        {
            const u32 divisor_shift = divisor >> 1;
            const u64 base = (u64)1 << (3 + (index >> divisor_shift));
            return (base + (base >> divisor_shift) * (index & (divisor - 1)) + 7) & ~(u64)7;
        }

        allocator_t* create_v3(u64 managed_size, u32 pool_size, const alloc_config_t* configs, u32 count, u32 minimum_size, u32 alignment, u32 D)
        {
            if (configs == nullptr || count == 0 || minimum_size != 8 || alignment != 8 || (D != 2 && D != 4) || count > D * (c2GiB - 3) + 1)
                return nullptr;

            const u32 page_size       = v_alloc_get_page_size();
            const u8  page_shift      = v_alloc_get_page_size_shift();
            const u64 min_region_size = (u64)1 << c16MiB;
            const u64 pool_total      = (u64)pool_size * 12;
            if (page_size == 0 || (page_size & (page_size - 1)) != 0 || page_size > ((u32)1 << c16KiB) || page_shift >= 32 || ((u32)1 << page_shift) != page_size || managed_size < min_region_size || (managed_size & (min_region_size - 1)) != 0 ||
                (u64)(uint_t)managed_size != managed_size || pool_size < 8192 || pool_size < page_size || (pool_size & (pool_size - 1)) != 0 || pool_total > 0xffffffffu || (u64)(uint_t)pool_total != pool_total)
                return nullptr;

            for (u32 i = 0; i < count; ++i)
            {
                const alloc_config_t& config = configs[i];
                if (config.m_alloc_size_index != i || config.m_region_size_shift < c16MiB || config.m_region_size_shift >= cShiftValues || config.m_chunk_size_shift < page_shift || config.m_chunk_size_shift > config.m_region_size_shift ||
                    config.m_region_size_shift - config.m_chunk_size_shift > cMaxRegionSlotsShift || config.m_item_cap_shift > 12)
                    return nullptr;
                if (s_item_size_for_bin(i, D) > ((u64)1 << config.m_chunk_size_shift))
                    return nullptr;
            }

            const u64 lookup_count = managed_size >> c16MiB;
            if (lookup_count > 0xffffffffu / sizeof(u32))
                return nullptr;

            const u64 max_config_bytes     = sizeof(allocator_t) + cConfigArraysBytes + ((u64)count * 3 + cMaxConfigs * 3 + lookup_count) * sizeof(u32);
            const u64 scratch_end          = sizeof(allocator_t) + cConfigArraysBytes + cPresenceBytes;
            const u64 required_reservation = max_config_bytes > scratch_end ? max_config_bytes : scratch_end;
            const u64 config_size          = (required_reservation + page_size - 1) & ~((u64)page_size - 1);
            const u64 initial_commit       = (scratch_end + page_size - 1) & ~((u64)page_size - 1);
            if (config_size > 0xffffffffu || (u64)(uint_t)config_size != config_size)
                return nullptr;

            byte* managed_memory_base = (byte*)v_alloc_reserve((uint_t)managed_size);
            if (managed_memory_base == nullptr)
                return nullptr;

            byte* arena_memory_base = (byte*)v_alloc_reserve((uint_t)pool_total);
            if (arena_memory_base == nullptr)
            {
                s_release_reservation(managed_memory_base, managed_size);
                return nullptr;
            }

            byte* config_memory_base = (byte*)v_alloc_reserve((uint_t)config_size);
            if (config_memory_base == nullptr || !v_alloc_commit(config_memory_base, (uint_t)initial_commit))
            {
                s_release_reservation(config_memory_base, config_size);
                s_release_reservation(arena_memory_base, pool_total);
                s_release_reservation(managed_memory_base, managed_size);
                return nullptr;
            }

            allocator_t* a = (allocator_t*)config_memory_base;

            byte*            cursor              = (byte*)(a + 1);
            chunk_config_t*  chunk_config_array  = s_advance_cursor<chunk_config_t>(cursor, cConfigSlots);
            region_config_t* region_config_array = s_advance_cursor<region_config_t>(cursor, cConfigSlots);
            bin_config_t*    bin_config_array    = s_advance_cursor<bin_config_t>(cursor, cConfigSlots);
            u8*              presence            = cursor;

            for (u32 i = 0; i < count; ++i)
            {
                const alloc_config_t& config           = configs[i];
                bin_config_array[i].m_alloc_size_index = config.m_alloc_size_index;
                bin_config_array[i].m_chunk_cfg_index  = 0xff;
                bin_config_array[i].m_region_cfg_index = 0xff;
                bin_config_array[i].m_reserved         = 0;
            }

            u8 chunk_count = 0;
            if (!s_build_chunk_configs(configs, count, chunk_config_array, presence, chunk_count))
            {
                s_release_reservation(config_memory_base, config_size);
                s_release_reservation(arena_memory_base, pool_total);
                s_release_reservation(managed_memory_base, managed_size);
                return nullptr;
            }
            s_build_bin_config_chunks(configs, count, bin_config_array, presence);

            u8 region_count = 0;
            if (!s_build_region_configs(configs, count, region_config_array, presence, region_count))
            {
                s_release_reservation(config_memory_base, config_size);
                s_release_reservation(arena_memory_base, pool_total);
                s_release_reservation(managed_memory_base, managed_size);
                return nullptr;
            }
            s_build_bin_config_regions(configs, count, bin_config_array, presence);

            const u64 config_bytes = sizeof(allocator_t) + cConfigArraysBytes + ((u64)count * 3 + chunk_count + 2 * region_count + lookup_count) * sizeof(u32);
            const u64 final_commit = (config_bytes + page_size - 1) & ~((u64)page_size - 1);
            if (final_commit > initial_commit && !v_alloc_commit(config_memory_base + initial_commit, (uint_t)(final_commit - initial_commit)))
            {
                s_release_reservation(config_memory_base, config_size);
                s_release_reservation(arena_memory_base, pool_total);
                s_release_reservation(managed_memory_base, managed_size);
                return nullptr;
            }

            nmem::memset(a, 0, sizeof(allocator_t));
            a->m_memory_base          = managed_memory_base;
            a->m_memory_size          = managed_size;
            a->m_arena_memory_base    = arena_memory_base;
            a->m_arena_memory_size    = pool_total;
            a->m_config_memory_base   = config_memory_base;
            a->m_config_memory_size   = config_size;
            a->m_chunk_config_array   = chunk_config_array;
            a->m_region_config_array  = region_config_array;
            a->m_bin_config_array     = bin_config_array;
            a->m_num_bin_configs      = (u16)count;
            a->m_num_chunk_configs    = chunk_count;
            a->m_num_region_configs   = region_count;
            a->m_size_class_divisor   = (u8)D;
            a->m_min_region_sizeshift = c16MiB;
            a->m_arena_size_sizeshift = math::ilog2(pool_size);

            cursor                                     = presence;
            a->m_not_full_o_chunks_per_alloc_index     = s_advance_cursor<u32>(cursor, count);
            a->m_cached_empty_o_chunks_per_chunk_index = s_advance_cursor<u32>(cursor, chunk_count);
            a->m_active_o_region_per_region_index      = s_advance_cursor<u32>(cursor, region_count);
            a->m_unused_o_region_per_region_index      = s_advance_cursor<u32>(cursor, region_count);
            a->m_item_size_per_alloc_index             = s_advance_cursor<u32>(cursor, count);
            a->m_item_capacity_per_alloc_index         = s_advance_cursor<u32>(cursor, count);
            a->m_o_region_lookup_array                 = s_advance_cursor<u32>(cursor, lookup_count);

            for (u32 i = 0; i < count; ++i)
            {
                a->m_not_full_o_chunks_per_alloc_index[i] = 0xffffffffu;
                const u64 size                            = s_item_size_for_bin(i, D);
                const u64 chunk_size                      = (u64)1 << configs[i].m_chunk_size_shift;
                const u32 bitmap_capacity                 = (u32)1 << configs[i].m_item_cap_shift;
                const u64 fitting_items                   = chunk_size / size;
                a->m_item_size_per_alloc_index[i]         = (u32)size;
                a->m_item_capacity_per_alloc_index[i]     = fitting_items < bitmap_capacity ? (u32)fitting_items : bitmap_capacity;
            }

            for (u16 i = 0; i < chunk_count; ++i)
                a->m_cached_empty_o_chunks_per_chunk_index[i] = 0xffffffffu;

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

            return a;
        }

        static inline u8        s_pointer_to_arena_index(allocator_t* a, void* ptr) { return (u8)(((byte*)ptr - a->m_arena_memory_base) >> a->m_arena_size_sizeshift); }
        static inline u32       s_region_to_offset(allocator_t* a, region_t* region) { return (u32)((byte*)region - a->m_arena_memory_base); }
        static inline region_t* s_offset_to_region(allocator_t* a, u32 offset) { return (region_t*)(a->m_arena_memory_base + offset); }
        static inline u32       s_chunk_to_offset(allocator_t* a, chunk_t* chunk) { return (u32)((byte*)chunk - a->m_arena_memory_base); }
        static inline chunk_t*  s_offset_to_chunk(allocator_t* a, u32 offset) { return (chunk_t*)(a->m_arena_memory_base + offset); }
        static inline u32       s_block_to_offset(allocator_t* a, block_t* block) { return (u32)((byte*)block - a->m_arena_memory_base); }
        static inline block_t*  s_offset_to_block(allocator_t* a, u32 offset) { return (block_t*)(a->m_arena_memory_base + offset); }
        static inline u32       s_array_to_offset(allocator_t* a, void* array) { return (u32)((byte*)array - a->m_arena_memory_base); }
        static inline u32*      s_offset_to_array(allocator_t* a, u32 offset) { return (u32*)(a->m_arena_memory_base + offset); }

        static chunk_t*  s_allocate_chunk_struct(allocator_t* a) { return (chunk_t*)s_allocate(&a->m_arenas[0]); }
        static block_t*  s_allocate_block_struct(allocator_t* a) { return (block_t*)s_allocate(&a->m_arenas[0]); }
        static region_t* s_allocate_region_struct(allocator_t* a) { return (region_t*)s_allocate(&a->m_arenas[0]); }

        static void* s_allocate_array(allocator_t* a, u32 size)
        {
            // 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
            // TODO optimize this using bit manipulation (e.g., find the index using log2(size) - 3)
            switch (size)
            {
                case 1:
                case 2:
                case 4:
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
                default: ASSERT(size <= 8192); break;
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
        static void s_free_chunk_struct(allocator_t* a, chunk_t* chunk) { s_free_pointer(a, (void*)chunk); }
        static void s_free_block_struct(allocator_t* a, block_t* block) { s_free_pointer(a, (void*)block); }
        static void s_free_region_struct(allocator_t* a, region_t* region) { s_free_pointer(a, (void*)region); }
        static void s_free_array(allocator_t* a, void* array) { s_free_pointer(a, array); }

        region_t* s_create_region(allocator_t* a, u8 region_config_index)
        {
            const region_config_t& config       = a->m_region_config_array[region_config_index];
            const u8               region_shift = config.m_region_size_shift;
            const u8               chunk_shift  = config.m_chunk_size_shift;

            const u32 capacity     = (u32)1 << (region_shift - chunk_shift);
            const u32 lookup_span  = (u32)1 << (region_shift - a->m_min_region_sizeshift);
            const u64 lookup_count = a->m_memory_size >> a->m_min_region_sizeshift;
            if ((u64)a->m_next_region_index + lookup_span > lookup_count)
                return nullptr;

            region_t* region = s_allocate_region_struct(a);
            if (region == nullptr)
                return nullptr;
            const u32 array_size = capacity * sizeof(u32);
            u32*      slots      = (u32*)s_allocate_array(a, array_size);
            if (slots == nullptr)
            {
                s_free_region_struct(a, region);
                return nullptr;
            }

            region->m_type             = 2;
            region->m_region_cfg_index = region_config_index;
            region->m_reserved0        = 0;
            region->m_count            = 0;
            region->m_capacity         = (u16)capacity;
            region->m_free_slot_index  = 0;
            region->m_free_list        = 0xffffu;
            region->m_index            = a->m_next_region_index;
            region->m_o_array          = s_array_to_offset(a, slots);
            region->m_o_next           = a->m_active_o_region_per_region_index[region_config_index];

            const u32 region_offset = s_region_to_offset(a, region);
            for (u32 i = 0; i < lookup_span; ++i)
                a->m_o_region_lookup_array[a->m_next_region_index + i] = region_offset;
            a->m_next_region_index += lookup_span;
            a->m_active_o_region_per_region_index[region_config_index] = region_offset;
            return region;
        }

        s32 s_pop_region_slot(allocator_t* a, region_t* region)
        {
            if (region->m_free_list != 0xffffu)
            {
                const u16 index = region->m_free_list;
                region->m_free_list = (u16)s_offset_to_array(a, region->m_o_array)[index];
                return index;
            }
            if (region->m_free_slot_index == region->m_capacity)
                return -1;
            return region->m_free_slot_index++;
        }

        void s_push_region_slot(allocator_t* a, region_t* region, u16 index)
        {
            s_offset_to_array(a, region->m_o_array)[index] = region->m_free_list;
            region->m_free_list = index;
        }

        static void s_assign_region_slot(allocator_t* a, region_t* region, u16 index, u32 offset)
        {
            u32* slots = s_offset_to_array(a, region->m_o_array);
            slots[index] = offset;
            if (++region->m_count == region->m_capacity)
                a->m_active_o_region_per_region_index[region->m_region_cfg_index] = region->m_o_next;
        }

        static byte* s_region_slot_address(allocator_t* a, region_t* region, u16 index)
        {
            const u8 slot_shift = a->m_region_config_array[region->m_region_cfg_index].m_chunk_size_shift;
            return a->m_memory_base + ((u64)region->m_index << a->m_min_region_sizeshift) + ((u64)index << slot_shift);
        }

        chunk_t* s_create_chunk(allocator_t* a, region_t* region, u8 bin_index)
        {
            chunk_t* chunk = s_allocate_chunk_struct(a);
            if (chunk == nullptr)
                return nullptr;

            const bin_config_t& bin   = a->m_bin_config_array[bin_index];
            const u8            shift = a->m_chunk_config_array[bin.m_chunk_cfg_index].m_item_cap_shift;
            void*               bin1  = nullptr;
            if (shift > 6)
            {
                bin1 = s_allocate_array(a, (u32)1 << (shift - 3));
                if (bin1 == nullptr)
                {
                    s_free_chunk_struct(a, chunk);
                    return nullptr;
                }
            }

            const bool recycled = region->m_free_list != 0xffffu;
            const s32 slot_index = s_pop_region_slot(a, region);
            ASSERT(slot_index >= 0);
            const u8  slot_shift = a->m_region_config_array[region->m_region_cfg_index].m_chunk_size_shift;
            if (!v_alloc_commit(s_region_slot_address(a, region, (u16)slot_index), (uint_t)1 << slot_shift))
            {
                if (recycled)
                    s_push_region_slot(a, region, (u16)slot_index);
                else
                    --region->m_free_slot_index;
                if (bin1 != nullptr)
                    s_free_array(a, bin1);
                s_free_chunk_struct(a, chunk);
                return nullptr;
            }

            chunk->m_type                                     = 0;
            chunk->m_alloc_cfg_index                          = bin_index;
            chunk->m_count                                    = 0;
            chunk->m_free_item_index                          = 0;
            chunk->m_bin0                                     = 0;
            chunk->m_o_bin1                                   = bin1 == nullptr ? 0xffffffffu : s_array_to_offset(a, bin1);
            chunk->m_o_tags                                   = 0xffffffffu;
            chunk->m_o_region                                 = s_region_to_offset(a, region);
            chunk->m_o_next                                   = a->m_not_full_o_chunks_per_alloc_index[bin_index];
            const u32 offset                                  = s_chunk_to_offset(a, chunk);
            chunk->m_index                                    = (u16)slot_index;
            s_assign_region_slot(a, region, chunk->m_index, offset);
            a->m_not_full_o_chunks_per_alloc_index[bin_index] = offset;
            return chunk;
        }

        block_t* s_create_block(allocator_t* a, region_t* region, u8 bin_index)
        {
            if (region->m_count == region->m_capacity)
                return nullptr;

            block_t* block = s_allocate_block_struct(a);
            if (block == nullptr)
                return nullptr;
            const s32 slot_index = s_pop_region_slot(a, region);
            if (slot_index < 0)
            {
                s_free_block_struct(a, block);
                return nullptr;
            }

            block->m_type            = 1;
            block->m_alloc_cfg_index = bin_index;
            block->m_pages           = 0;
            block->m_o_region        = s_region_to_offset(a, region);
            block->m_tag             = 0;
            block->m_o_next          = 0xffffffffu;
            block->m_index           = (u16)slot_index;
            s_assign_region_slot(a, region, block->m_index, s_block_to_offset(a, block));
            return block;
        }

        bool s_free_block(allocator_t* a, block_t* block)
        {
            region_t* region = s_offset_to_region(a, block->m_o_region);
            if (block->m_pages != 0)
            {
                byte* address = s_region_slot_address(a, region, block->m_index);
                const uint_t size = (uint_t)block->m_pages << a->m_arenas[0].m_page_sizeshift;
                if (!v_alloc_decommit(address, size))
                    return false;
            }

            s_push_region_slot(a, region, block->m_index);
            if (region->m_count == region->m_capacity)
            {
                region->m_o_next = a->m_active_o_region_per_region_index[region->m_region_cfg_index];
                a->m_active_o_region_per_region_index[region->m_region_cfg_index] = s_region_to_offset(a, region);
            }
            --region->m_count;
            s_free_block_struct(a, block);
            return true;
        }

        void* s_allocate_chunk_item(allocator_t* a, chunk_t* chunk)
        {
            const u8  bin_index = chunk->m_alloc_cfg_index;
            const u32 capacity  = a->m_item_capacity_per_alloc_index[bin_index];
            if (chunk->m_count == capacity)
                return nullptr;

            region_t* region        = s_offset_to_region(a, chunk->m_o_region);
            byte*     chunk_address = s_region_slot_address(a, region, chunk->m_index);
            const u32 item_size     = a->m_item_size_per_alloc_index[bin_index];

            const bool two_levels = chunk->m_o_bin1 != 0xffffffffu;
            u64*       bin1       = two_levels ? (u64*)s_offset_to_array(a, chunk->m_o_bin1) : nullptr;
            const s32  free_item  = two_levels ? nbitvec12::find_free_and_remove(&chunk->m_bin0, bin1, capacity) : nbitvec6::find_free_and_remove(&chunk->m_bin0, capacity);
            u32        index;
            if (free_item >= 0)
            {
                index = (u32)free_item;
            }
            else
            {
                index = chunk->m_free_item_index++;
                if (two_levels)
                {
                    nbitvec12::tick_used_lazy(&chunk->m_bin0, bin1, capacity, index);
                }
            }
            if (++chunk->m_count == capacity)
                a->m_not_full_o_chunks_per_alloc_index[bin_index] = chunk->m_o_next;
            return chunk_address + (u64)index * item_size;
        }

        void s_free_chunk_item(allocator_t* a, chunk_t* chunk, void* item)
        {
            const u8  bin_index     = chunk->m_alloc_cfg_index;
            const u32 capacity      = a->m_item_capacity_per_alloc_index[bin_index];
            const u32 item_size     = a->m_item_size_per_alloc_index[bin_index];
            region_t* region        = s_offset_to_region(a, chunk->m_o_region);
            byte*     chunk_address = s_region_slot_address(a, region, chunk->m_index);
            ASSERT((uint_t)item >= (uint_t)chunk_address);
            const uint_t offset = (uint_t)item - (uint_t)chunk_address;
            ASSERT(offset % item_size == 0);
            const u32 index = (u32)(offset / item_size);
            ASSERT(offset / item_size < chunk->m_free_item_index);
            ASSERT(chunk->m_count > 0);

            if (chunk->m_o_bin1 != 0xffffffffu)
            {
                u64* bin1 = (u64*)s_offset_to_array(a, chunk->m_o_bin1);
                ASSERT(!nbitvec12::get(&chunk->m_bin0, bin1, capacity, index));
                nbitvec12::set_free(&chunk->m_bin0, bin1, capacity, index);
            }
            else
            {
                ASSERT(!nbitvec6::get(&chunk->m_bin0, capacity, index));
                nbitvec6::set_free(&chunk->m_bin0, capacity, index);
            }
            if (chunk->m_count == capacity)
            {
                chunk->m_o_next                                   = a->m_not_full_o_chunks_per_alloc_index[bin_index];
                a->m_not_full_o_chunks_per_alloc_index[bin_index] = s_chunk_to_offset(a, chunk);
            }
            chunk->m_count -= 1;
        }

        const bin_config_t* s_get_bin_config_for_size(const allocator_t* a, u32 alloc_size)
        {
            if (alloc_size == 0 || alloc_size > ((u32)1 << c2GiB))
                return nullptr;

            const u32 size = (alloc_size + 7) & ~(u32)7;

            const u32 value         = size - 1;
            const u32 exponent      = (u32)math::ilog2(value);
            const u32 divisor_shift = a->m_size_class_divisor == 2 ? 1 : 2;
            const u32 offset        = ((value - ((u32)1 << exponent)) >> (exponent - divisor_shift)) + 1;
            const u32 bin           = ((exponent - 3) << divisor_shift) + offset;
            return bin < a->m_num_bin_configs ? &a->m_bin_config_array[bin] : nullptr;
        }

        void* allocate(allocator_t* a, u32 size)
        {
            // - const bin_config_t* bc = s_get_bin_config_for_size(a, size);
            // - Is there an active chunk for this allocation config
            //   - If not, check if there is an active region for this allocation config
            //     - If not, allocate and initialize a new region for the allocation configuration
            //       - Add the new region to the active region list
            //     - If yes, allocate and initialize a new chunk within the active region
            //       - Add the chunk to the active chunk list
            // - Allocate item from the active chunk
            // - If chunk is now full remove it from the active list
            // - return item

            return nullptr;
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
