#include "ccore/c_memory.h"
#include "cunittest/cunittest.h"

#include "csuperalloc/c_superalloc_v3.h"

using namespace ncore;
using namespace ncore::nsuperallocv3;

static const alloc_config_t s_test_configs[] = {
    {0, 24, 14, 11}, {1, 24, 14, 10}, {2, 24, 14, 10}, {3, 24, 14, 10},
    {4, 24, 15, 10}, {5, 24, 15, 10}, {6, 24, 16, 10}, {7, 25, 20, 0},
};

static bool initialize_test(allocator_t* a, u64 managed_size, u32 pool_size)
{
    return initialize(a, managed_size, pool_size, s_test_configs, DARRAYSIZE(s_test_configs), 8, 8, 2);
}

UNITTEST_SUITE_BEGIN(superalloc_v3)
{
    UNITTEST_FIXTURE(initialization)
    {
        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(rejects_invalid_sizes)
        {
            allocator_t a = {};
            const u64 region_size = (u64)1 << 24;
            const u32 pool_size = 32768;
            CHECK_FALSE(initialize_test(nullptr, region_size, pool_size));
            CHECK_FALSE(initialize_test(&a, 0, pool_size));
            CHECK_FALSE(initialize_test(&a, region_size + 1, pool_size));
            CHECK_FALSE(initialize_test(&a, region_size, 0));
            CHECK_FALSE(initialize_test(&a, region_size, 24576));
            CHECK_FALSE(initialize_test(&a, region_size, (u32)1 << 29));
            CHECK_TRUE(a.m_memory_base == nullptr);
            CHECK_TRUE(a.m_arena_memory_base == nullptr);
            CHECK_TRUE(a.m_config_memory_base == nullptr);
            CHECK_TRUE(shutdown(&a));
            CHECK_FALSE(shutdown(nullptr));
        }

        UNITTEST_TEST(initializes_configurations_and_pools)
        {
            allocator_t a = {};
            const u32 pool_size = 32768;
            const bool initialized = initialize_test(&a, (u64)2 << 24, pool_size);
            CHECK_TRUE(initialized);
            if (!initialized)
                return;

            CHECK_FALSE(initialize_test(&a, (u64)1 << 24, pool_size));
            CHECK_EQUAL((u16)DARRAYSIZE(s_test_configs), a.m_num_bin_configs);
            CHECK_TRUE(a.m_num_chunk_configs > 0);
            CHECK_TRUE(a.m_num_region_configs > 0);
            CHECK_EQUAL((s8)24, a.m_min_region_sizeshift);
            CHECK_EQUAL((s8)15, a.m_arena_size_sizeshift);
            CHECK_EQUAL((u64)pool_size * 12, a.m_arena_memory_size);
            CHECK_TRUE((byte*)a.m_chunk_config_array == a.m_config_memory_base);
            CHECK_TRUE((byte*)a.m_region_config_array == a.m_config_memory_base + 256 * sizeof(chunk_config_t));
            CHECK_TRUE((byte*)a.m_bin_config_array == a.m_config_memory_base + 2048);
            CHECK_EQUAL((u32)0xffffffff, a.m_o_region_lookup_array[0]);
            CHECK_EQUAL((u32)0xffffffff, a.m_o_region_lookup_array[1]);

            u32 next_chunk = 0;
            u32 next_region = 0;
            for (u32 i = 0; i < a.m_num_bin_configs; ++i)
            {
                const alloc_config_t& config = s_test_configs[i];
                const bin_config_t& bin = a.m_bin_config_array[i];
                CHECK_EQUAL((u8)i, bin.m_alloc_size_index);
                CHECK_EQUAL((u8)0, bin.m_reserved);
                CHECK_EQUAL((u32)0xffffffff, a.m_not_full_o_chunks_per_alloc_index[config.m_alloc_size_index]);
                CHECK_TRUE(bin.m_chunk_cfg_index < a.m_num_chunk_configs);
                CHECK_TRUE(bin.m_region_cfg_index < a.m_num_region_configs);
                if (bin.m_chunk_cfg_index < a.m_num_chunk_configs && bin.m_region_cfg_index < a.m_num_region_configs)
                {
                    const chunk_config_t& chunk = a.m_chunk_config_array[bin.m_chunk_cfg_index];
                    const region_config_t& region = a.m_region_config_array[bin.m_region_cfg_index];
                    CHECK_EQUAL(config.m_chunk_size_shift, chunk.m_chunk_size_shift);
                    CHECK_EQUAL(config.m_item_cap_shift, chunk.m_item_cap_shift);
                    CHECK_EQUAL(config.m_region_size_shift, region.m_region_size_shift);
                    CHECK_EQUAL(config.m_chunk_size_shift, region.m_chunk_size_shift);
                }
                u32 chunks = 0;
                u32 regions = 0;
                bool previous_chunk = false;
                bool previous_region = false;
                for (u32 j = 0; j < i; ++j)
                {
                    const alloc_config_t& previous = s_test_configs[j];
                    previous_chunk |= previous.m_chunk_size_shift == config.m_chunk_size_shift &&
                                      previous.m_item_cap_shift == config.m_item_cap_shift;
                    previous_region |= previous.m_region_size_shift == config.m_region_size_shift &&
                                       previous.m_chunk_size_shift == config.m_chunk_size_shift;
                }
                if (!previous_chunk)
                {
                    CHECK_EQUAL(config.m_chunk_size_shift, a.m_chunk_config_array[next_chunk].m_chunk_size_shift);
                    CHECK_EQUAL(config.m_item_cap_shift, a.m_chunk_config_array[next_chunk].m_item_cap_shift);
                    ++next_chunk;
                }
                if (!previous_region)
                {
                    CHECK_EQUAL(config.m_region_size_shift, a.m_region_config_array[next_region].m_region_size_shift);
                    CHECK_EQUAL(config.m_chunk_size_shift, a.m_region_config_array[next_region].m_chunk_size_shift);
                    ++next_region;
                }
                for (u32 j = 0; j < a.m_num_chunk_configs; ++j)
                    if (a.m_chunk_config_array[j].m_chunk_size_shift == config.m_chunk_size_shift &&
                        a.m_chunk_config_array[j].m_item_cap_shift == config.m_item_cap_shift)
                        ++chunks;
                for (u32 j = 0; j < a.m_num_region_configs; ++j)
                    if (a.m_region_config_array[j].m_region_size_shift == config.m_region_size_shift &&
                        a.m_region_config_array[j].m_chunk_size_shift == config.m_chunk_size_shift)
                        ++regions;
                CHECK_EQUAL((u32)1, chunks);
                CHECK_EQUAL((u32)1, regions);
            }
            CHECK_EQUAL((u32)a.m_num_chunk_configs, next_chunk);
            CHECK_EQUAL((u32)a.m_num_region_configs, next_region);
            for (u32 i = 0; i < a.m_num_chunk_configs; ++i)
            {
                CHECK_EQUAL((u32)0xffffffff, a.m_cached_empty_o_chunks_per_chunk_index[i]);
            }
            for (u32 i = 0; i < a.m_num_region_configs; ++i)
            {
                CHECK_EQUAL((u32)0xffffffff, a.m_active_o_region_per_region_index[i]);
                CHECK_EQUAL((u32)0xffffffff, a.m_unused_o_region_per_region_index[i]);
            }
            for (u32 i = 0; i < 12; ++i)
            {
                const arena_pool_t& pool = a.m_arenas[i];
                CHECK_TRUE(pool.m_memory_base == a.m_arena_memory_base + i * pool_size);
                CHECK_TRUE(pool.m_memory_cursor == pool.m_memory_base);
                CHECK_EQUAL((u16)(i == 0 ? 32 : (u32)1 << (i + 2)), pool.m_allocation_size);
                CHECK_EQUAL((u32)0, pool.m_committed_pages);
                CHECK_EQUAL((u32)(pool_size / v_alloc_get_page_size()), pool.m_reserved_pages);
                CHECK_EQUAL((u32)0xffffffff, pool.m_free_list);
            }

            CHECK_TRUE(shutdown(&a));
            CHECK_TRUE(a.m_memory_base == nullptr);
            CHECK_TRUE(a.m_arena_memory_base == nullptr);
            CHECK_TRUE(a.m_config_memory_base == nullptr);
            CHECK_TRUE(shutdown(&a));
            const bool reinitialized = initialize_test(&a, (u64)1 << 24, pool_size);
            CHECK_TRUE(reinitialized);
            if (reinitialized)
                CHECK_TRUE(shutdown(&a));
        }

        UNITTEST_TEST(configuration_presence_table_bounds)
        {
            alloc_config_t  configs[256] = {};
            chunk_config_t  chunks[256]  = {};
            region_config_t regions[256] = {};
            u8              presence[32 * 32];
            u16             chunk_count  = 0;
            u16             region_count = 0;

            configs[0].m_chunk_size_shift  = 31;
            configs[0].m_item_cap_shift    = 31;
            configs[0].m_region_size_shift = 31;
            CHECK_TRUE(s_build_chunk_configs(configs, 1, chunks, presence, chunk_count));
            CHECK_TRUE(s_build_region_configs(configs, 1, regions, presence, region_count));
            configs[0].m_chunk_size_shift = 32;
            CHECK_FALSE(s_build_chunk_configs(configs, 1, chunks, presence, chunk_count));
            CHECK_FALSE(s_build_region_configs(configs, 1, regions, presence, region_count));
            configs[0].m_chunk_size_shift = 0;
            configs[0].m_item_cap_shift   = 32;
            CHECK_FALSE(s_build_chunk_configs(configs, 1, chunks, presence, chunk_count));
            configs[0].m_item_cap_shift    = 0;
            configs[0].m_region_size_shift = 32;
            CHECK_FALSE(s_build_region_configs(configs, 1, regions, presence, region_count));

            for (u32 i = 0; i < 256; ++i)
            {
                configs[i].m_region_size_shift = 0;
                configs[i].m_chunk_size_shift  = (u8)(i / 32);
                configs[i].m_item_cap_shift    = (u8)(i % 32);
            }
            CHECK_TRUE(s_build_chunk_configs(configs, 255, chunks, presence, chunk_count));
            CHECK_EQUAL((u16)255, chunk_count);
            CHECK_FALSE(s_build_chunk_configs(configs, 256, chunks, presence, chunk_count));

            for (u32 i = 0; i < 256; ++i)
            {
                configs[i].m_region_size_shift = (u8)(i / 32);
                configs[i].m_chunk_size_shift  = (u8)(i % 32);
                configs[i].m_item_cap_shift    = 0;
            }
            CHECK_TRUE(s_build_region_configs(configs, 255, regions, presence, region_count));
            CHECK_EQUAL((u16)255, region_count);
            CHECK_FALSE(s_build_region_configs(configs, 256, regions, presence, region_count));
        }

        UNITTEST_TEST(rejects_invalid_configuration_inputs)
        {
            allocator_t a = {};
            alloc_config_t configs[2] = {{0, 24, 14, 11}, {1, 24, 14, 10}};
            const u64 size = (u64)1 << 24;
            CHECK_FALSE(initialize(&a, size, 32768, nullptr, 1, 8, 8, 2));
            CHECK_FALSE(initialize(&a, size, 32768, configs, 0, 8, 8, 2));
            CHECK_FALSE(initialize(&a, size, 32768, configs, 58, 8, 8, 2));
            CHECK_FALSE(initialize(&a, size, 32768, configs, 114, 8, 8, 4));
            CHECK_FALSE(initialize(&a, size, 32768, configs, 2, 16, 8, 2));
            CHECK_FALSE(initialize(&a, size, 32768, configs, 2, 8, 16, 2));
            CHECK_FALSE(initialize(&a, size, 32768, configs, 2, 8, 8, 3));
            configs[1].m_alloc_size_index = 2;
            CHECK_FALSE(initialize(&a, size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_alloc_size_index = 1;
            configs[1].m_region_size_shift = 32;
            CHECK_FALSE(initialize(&a, size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_region_size_shift = 24;
            configs[1].m_chunk_size_shift = 32;
            CHECK_FALSE(initialize(&a, size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_chunk_size_shift = 14;
            configs[1].m_item_cap_shift = 32;
            CHECK_FALSE(initialize(&a, size, 32768, configs, 2, 8, 8, 2));
            CHECK_TRUE(a.m_memory_base == nullptr);
            CHECK_TRUE(a.m_arena_memory_base == nullptr);
            CHECK_TRUE(a.m_config_memory_base == nullptr);
            CHECK_TRUE(shutdown(&a));
        }

        UNITTEST_TEST(size_classes_and_caller_owned_input)
        {
            alloc_config_t configs[113];
            for (u32 i = 0; i < DARRAYSIZE(configs); ++i)
                configs[i] = {(u8)i, 24, 14, 11};

            for (u32 D = 2; D <= 4; D += 2)
            {
                allocator_t a = {};
                const u32 count = D * 28 + 1;
                const bool initialized = initialize(&a, (u64)1 << 24, 32768, configs, count, 8, 8, D);
                CHECK_TRUE(initialized);
                if (!initialized)
                    continue;

                CHECK_TRUE(s_get_bin_config_for_size(&a, 1) == &a.m_bin_config_array[0]);
                CHECK_TRUE(s_get_bin_config_for_size(&a, 8) == &a.m_bin_config_array[0]);
                CHECK_TRUE(s_get_bin_config_for_size(&a, 9) == &a.m_bin_config_array[D]);
                CHECK_TRUE(s_get_bin_config_for_size(&a, 17) == &a.m_bin_config_array[D + D / 2]);
                CHECK_TRUE(s_get_bin_config_for_size(&a, ((u32)1 << 31) - 1) == &a.m_bin_config_array[count - 1]);
                CHECK_TRUE(s_get_bin_config_for_size(&a, (u32)1 << 31) == &a.m_bin_config_array[count - 1]);
                CHECK_TRUE(s_get_bin_config_for_size(&a, ((u32)1 << 31) + 1) == nullptr);
                CHECK_TRUE(s_get_bin_config_for_size(&a, 0xffffffffu) == nullptr);
                CHECK_TRUE(s_get_bin_config_for_size(&a, 0) == nullptr);
                configs[0].m_item_cap_shift = 7;
                CHECK_EQUAL((u8)11, a.m_chunk_config_array[a.m_bin_config_array[0].m_chunk_cfg_index].m_item_cap_shift);
                configs[0].m_item_cap_shift = 11;
                CHECK_TRUE(shutdown(&a));
            }

            allocator_t a = {};
            const bool initialized = initialize(&a, (u64)1 << 24, 32768, configs, 1, 8, 8, 4);
            CHECK_TRUE(initialized);
            if (initialized)
            {
                CHECK_TRUE(s_get_bin_config_for_size(&a, 8) == &a.m_bin_config_array[0]);
                CHECK_TRUE(s_get_bin_config_for_size(&a, 9) == nullptr);
                CHECK_TRUE(shutdown(&a));
            }
        }

        UNITTEST_TEST(configuration_commit_grows_for_large_lookup)
        {
            if (sizeof(uint_t) < sizeof(u64))
                return;

            const u64   page_size      = v_alloc_get_page_size();
            const u64   initial_commit = (2048 + 32 * 32 + page_size - 1) & ~(page_size - 1);
            const u64   lookup_count   = initial_commit / sizeof(u32) + 1;
            allocator_t a              = {};
            const bool  initialized    = initialize_test(&a, lookup_count << 24, 32768);
            CHECK_TRUE(initialized);
            if (!initialized)
                return;

            CHECK_TRUE((byte*)(a.m_o_region_lookup_array + lookup_count) > a.m_config_memory_base + initial_commit);
            CHECK_EQUAL((u32)0xffffffff, a.m_o_region_lookup_array[0]);
            CHECK_EQUAL((u32)0xffffffff, a.m_o_region_lookup_array[lookup_count - 1]);
            CHECK_TRUE(shutdown(&a));
        }

        UNITTEST_TEST(pool_commits_grows_reuses_and_stops_at_capacity)
        {
            allocator_t a = {};
            const u32 pool_size = 32768;
            const bool initialized = initialize_test(&a, (u64)1 << 24, pool_size);
            CHECK_TRUE(initialized);
            if (!initialized)
                return;

            arena_pool_t* pool = &a.m_arenas[0];
            const u32 page_size = v_alloc_get_page_size();
            const u32 count = pool_size / pool->m_allocation_size;
#if defined(TARGET_MAC) || defined(TARGET_LINUX)
            arena_pool_t invalid = *pool;
            ++invalid.m_memory_base;
            ++invalid.m_memory_cursor;
            CHECK_TRUE(s_allocate(&invalid) == nullptr);
            CHECK_TRUE(invalid.m_memory_cursor == invalid.m_memory_base);
            CHECK_EQUAL((u32)0, invalid.m_committed_pages);
#endif
            void* first = nullptr;
            for (u32 i = 0; i < count; ++i)
            {
                byte* item = (byte*)s_allocate(pool);
                CHECK_TRUE(item != nullptr);
                if (item == nullptr)
                    break;
                if (i == 0)
                    first = item;
                CHECK_TRUE(item == pool->m_memory_base + i * pool->m_allocation_size);
                item[0] = (byte)i;
                item[pool->m_allocation_size - 1] = (byte)i;
                CHECK_EQUAL((u32)(((i + 1) * pool->m_allocation_size + page_size - 1) / page_size), pool->m_committed_pages);
            }
            CHECK_TRUE(s_allocate(pool) == nullptr);
            CHECK_TRUE(pool->m_memory_cursor == pool->m_memory_base + pool_size);
            if (first != nullptr)
            {
                s_free(pool, first);
                CHECK_TRUE(s_allocate(pool) == first);
                CHECK_TRUE(s_allocate(pool) == nullptr);
            }
            arena_pool_t* large_pool = &a.m_arenas[11];
            byte* large = (byte*)s_allocate(large_pool);
            CHECK_TRUE(large != nullptr);
            if (large != nullptr)
            {
                large[0] = 1;
                large[8191] = 2;
                CHECK_EQUAL((u32)((8192 + page_size - 1) / page_size), large_pool->m_committed_pages);
            }
            CHECK_TRUE(shutdown(&a));
        }
    }
}
UNITTEST_SUITE_END
