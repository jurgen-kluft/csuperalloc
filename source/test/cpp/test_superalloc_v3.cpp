#include "ccore/c_memory.h"
#include "cunittest/cunittest.h"

#include "csuperalloc/c_superalloc_v3.h"

#if defined(TARGET_MAC) || defined(TARGET_LINUX)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ncore;
using namespace ncore::nsuperallocv3;

#if defined(TARGET_MAC) || defined(TARGET_LINUX)
static void exit_on_access_fault(int) { _exit(1); }

static bool page_is_inaccessible(const byte* address)
{
    const pid_t child = fork();
    if (child == 0)
    {
        signal(SIGSEGV, exit_on_access_fault);
        signal(SIGBUS, exit_on_access_fault);
        const volatile byte value = *(const volatile byte*)address;
        (void)value;
        _exit(0);
    }
    if (child < 0)
        return false;
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 1;
}
#endif

static const alloc_config_t s_test_configs[] = {
    {0, 24, 14, 11}, {1, 24, 14, 10}, {2, 24, 14, 10}, {3, 24, 14, 10},
    {4, 24, 15, 10}, {5, 24, 15, 10}, {6, 24, 16, 10}, {7, 25, 20, 0},
};

static allocator_t* initialize_test(u64 managed_size, u32 pool_size)
{
    return create_allocator(managed_size, pool_size, s_test_configs, DARRAYSIZE(s_test_configs), 8, 8, 2);
}

static chunk2_t* chunk_info(allocator_t* a, chunk_t* chunk)
{
    return (chunk2_t*)(a->m_arena_memory_base + chunk->m_o_chunk2);
}

static const bin_config_t* reference_bin(const allocator_t* a, u32 request)
{
    if (request == 0)
        return nullptr;
    const u64 size = ((u64)request + 7) & ~(u64)7;
    for (u32 i = 0; i < a->m_num_bin_configs; ++i)
    {
        const u32 exponent = 3 + i / a->m_size_class_divisor;
        const u64 base = (u64)1 << exponent;
        const u64 class_size = base + (base / a->m_size_class_divisor) * (i % a->m_size_class_divisor);
        if (size <= class_size)
            return &a->m_bin_config_array[i];
    }
    return nullptr;
}

UNITTEST_SUITE_BEGIN(superalloc_v3)
{
    UNITTEST_FIXTURE(initialization)
    {
        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(rejects_invalid_sizes)
        {
            const u64 region_size = (u64)1 << 24;
            const u32 pool_size = 32768;
            CHECK_TRUE(initialize_test(0, pool_size) == nullptr);
            CHECK_TRUE(initialize_test(region_size + 1, pool_size) == nullptr);
            CHECK_TRUE(initialize_test(region_size, 0) == nullptr);
            CHECK_TRUE(initialize_test(region_size, 24576) == nullptr);
            CHECK_TRUE(initialize_test(region_size, (u32)1 << 29) == nullptr);
            CHECK_FALSE(shutdown(nullptr));
        }

        UNITTEST_TEST(initializes_configurations_and_pools)
        {
            const u32 pool_size = 32768;
            allocator_t* a = initialize_test((u64)2 << 24, pool_size);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;

            CHECK_EQUAL((u16)DARRAYSIZE(s_test_configs), a->m_num_bin_configs);
            CHECK_TRUE(a->m_num_chunk_configs > 0);
            CHECK_TRUE(a->m_num_region_configs > 0);
            CHECK_EQUAL((s8)24, a->m_min_region_sizeshift);
            CHECK_EQUAL((s8)15, a->m_arena_size_sizeshift);
            CHECK_EQUAL((u64)pool_size * 12, a->m_arena_memory_size);
            CHECK_TRUE((byte*)a == a->m_config_memory_base);
            CHECK_TRUE((byte*)a->m_chunk_config_array == (byte*)(a + 1));
            CHECK_TRUE((byte*)a->m_region_config_array == (byte*)(a + 1) + 256 * sizeof(chunk_config_t));
            CHECK_TRUE((byte*)a->m_bin_config_array == (byte*)(a + 1) + 256 * (sizeof(chunk_config_t) + sizeof(region_config_t)));
            CHECK_TRUE((byte*)(a->m_item_info_per_alloc_index + a->m_num_bin_configs) == (byte*)a->m_o_region_lookup_array);
            CHECK_EQUAL((u32)0xffffffff, a->m_o_region_lookup_array[0]);
            CHECK_EQUAL((u32)0xffffffff, a->m_o_region_lookup_array[1]);

            u32 next_chunk = 0;
            u32 next_region = 0;
            for (u32 i = 0; i < a->m_num_bin_configs; ++i)
            {
                const alloc_config_t& config = s_test_configs[i];
                const bin_config_t& bin = a->m_bin_config_array[i];
                CHECK_EQUAL((u8)i, bin.m_alloc_size_index);
                CHECK_EQUAL((u8)0, bin.m_reserved);
                CHECK_EQUAL((u32)0xffffffff, a->m_not_full_o_chunks_per_alloc_index[config.m_alloc_size_index]);
                CHECK_TRUE(bin.m_chunk_cfg_index < a->m_num_chunk_configs);
                CHECK_TRUE(bin.m_region_cfg_index < a->m_num_region_configs);
                if (bin.m_chunk_cfg_index < a->m_num_chunk_configs && bin.m_region_cfg_index < a->m_num_region_configs)
                {
                    const chunk_config_t& chunk = a->m_chunk_config_array[bin.m_chunk_cfg_index];
                    const region_config_t& region = a->m_region_config_array[bin.m_region_cfg_index];
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
                    CHECK_EQUAL(config.m_chunk_size_shift, a->m_chunk_config_array[next_chunk].m_chunk_size_shift);
                    CHECK_EQUAL(config.m_item_cap_shift, a->m_chunk_config_array[next_chunk].m_item_cap_shift);
                    ++next_chunk;
                }
                if (!previous_region)
                {
                    CHECK_EQUAL(config.m_region_size_shift, a->m_region_config_array[next_region].m_region_size_shift);
                    CHECK_EQUAL(config.m_chunk_size_shift, a->m_region_config_array[next_region].m_chunk_size_shift);
                    ++next_region;
                }
                for (u32 j = 0; j < a->m_num_chunk_configs; ++j)
                    if (a->m_chunk_config_array[j].m_chunk_size_shift == config.m_chunk_size_shift &&
                        a->m_chunk_config_array[j].m_item_cap_shift == config.m_item_cap_shift)
                        ++chunks;
                for (u32 j = 0; j < a->m_num_region_configs; ++j)
                    if (a->m_region_config_array[j].m_region_size_shift == config.m_region_size_shift &&
                        a->m_region_config_array[j].m_chunk_size_shift == config.m_chunk_size_shift)
                        ++regions;
                CHECK_EQUAL((u32)1, chunks);
                CHECK_EQUAL((u32)1, regions);
            }
            CHECK_EQUAL((u32)a->m_num_chunk_configs, next_chunk);
            CHECK_EQUAL((u32)a->m_num_region_configs, next_region);
            for (u32 i = 0; i < a->m_num_chunk_configs; ++i)
            {
                CHECK_EQUAL((u32)0xffffffff, a->m_cached_empty_o_chunks_per_chunk_index[i]);
            }
            for (u32 i = 0; i < a->m_num_region_configs; ++i)
            {
                CHECK_EQUAL((u32)0xffffffff, a->m_active_o_region_per_region_index[i]);
                CHECK_EQUAL((u32)0xffffffff, a->m_unused_o_region_per_region_index[i]);
            }
            for (u32 i = 0; i < 12; ++i)
            {
                const arena_pool_t& pool = a->m_arenas[i];
                CHECK_TRUE(pool.m_memory_base == a->m_arena_memory_base + i * pool_size);
                CHECK_TRUE(pool.m_memory_cursor == pool.m_memory_base);
                CHECK_EQUAL((u16)(i == 0 ? 24 : (u32)1 << (i + 2)), pool.m_allocation_size);
                CHECK_EQUAL((u32)0, pool.m_committed_pages);
                CHECK_EQUAL((u32)(pool_size / v_alloc_get_page_size()), pool.m_reserved_pages);
                CHECK_EQUAL((u32)0xffffffff, pool.m_free_list);
            }

            CHECK_TRUE(shutdown(a));
            a = initialize_test((u64)1 << 24, pool_size);
            CHECK_TRUE(a != nullptr);
            if (a != nullptr)
                CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(configuration_presence_table_bounds)
        {
            alloc_config_t  configs[256] = {};
            chunk_config_t  chunks[256]  = {};
            region_config_t regions[256] = {};
            u8              presence[32 * 32];
            u8              chunk_count  = 0;
            u8              region_count = 0;

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
            CHECK_EQUAL((u8)255, chunk_count);
            CHECK_FALSE(s_build_chunk_configs(configs, 256, chunks, presence, chunk_count));

            for (u32 i = 0; i < 256; ++i)
            {
                configs[i].m_region_size_shift = (u8)(i / 32);
                configs[i].m_chunk_size_shift  = (u8)(i % 32);
                configs[i].m_item_cap_shift    = 0;
            }
            CHECK_TRUE(s_build_region_configs(configs, 255, regions, presence, region_count));
            CHECK_EQUAL((u8)255, region_count);
            CHECK_FALSE(s_build_region_configs(configs, 256, regions, presence, region_count));
        }

        UNITTEST_TEST(rejects_invalid_configuration_inputs)
        {
            alloc_config_t configs[114] = {{0, 24, 14, 11}, {1, 24, 14, 10}};
            const u64 size = (u64)1 << 24;
            CHECK_NULL(create_allocator(size, 32768, nullptr, 1, 8, 8, 2));
            CHECK_NULL(create_allocator(size, 32768, configs, 0, 8, 8, 2));
            CHECK_NULL(create_allocator(size, 32768, configs, 58, 8, 8, 2));
            CHECK_NULL(create_allocator(size, 32768, configs, 114, 8, 8, 4));
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 16, 8, 2));
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 16, 2));
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 8, 3));
            configs[1].m_alloc_size_index = 2;
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_alloc_size_index = 1;
            configs[1].m_region_size_shift = 32;
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_region_size_shift = 24;
            configs[1].m_chunk_size_shift = 32;
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_chunk_size_shift = 14;
            configs[1].m_item_cap_shift = 32;
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_item_cap_shift = 13;
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_item_cap_shift = 10;
            configs[1].m_region_size_shift = 23;
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_region_size_shift = 24;
            configs[1].m_chunk_size_shift = v_alloc_get_page_size_shift() - 1;
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_chunk_size_shift = 25;
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 8, 2));
            configs[1].m_region_size_shift = 28;
            configs[1].m_chunk_size_shift = 16;
            CHECK_NULL(create_allocator(size, 32768, configs, 2, 8, 8, 2));
        }

        UNITTEST_TEST(rejects_item_sizes_larger_than_configured_slots)
        {
            alloc_config_t configs[46];
            for (u32 i = 0; i < DARRAYSIZE(configs); ++i)
                configs[i] = {(u8)i, 24, 14, 12};

            for (u32 D = 2; D <= 4; D += 2)
            {
                const u32 boundary_index = 11 * D;
                allocator_t* a = create_allocator((u64)1 << 24, 32768, configs, boundary_index + 1, 8, 8, D);
                CHECK_TRUE(a != nullptr);
                if (a != nullptr)
                {
                    CHECK_EQUAL((u32)1 << 14, a->m_item_info_per_alloc_index[boundary_index].m_item_size);
                    CHECK_EQUAL((u32)1, a->m_item_info_per_alloc_index[boundary_index].m_item_capacity);
                    CHECK_TRUE(shutdown(a));
                }

                CHECK_NULL(create_allocator((u64)1 << 24, 32768, configs, boundary_index + 2, 8, 8, D));
                configs[boundary_index + 1].m_chunk_size_shift = 15;
                a = create_allocator((u64)1 << 24, 32768, configs, boundary_index + 2, 8, 8, D);
                CHECK_TRUE(a != nullptr);
                if (a != nullptr)
                {
                    CHECK_EQUAL((u32)((1 << 14) + (1 << 14) / D), a->m_item_info_per_alloc_index[boundary_index + 1].m_item_size);
                    CHECK_EQUAL((u32)1, a->m_item_info_per_alloc_index[boundary_index + 1].m_item_capacity);
                    CHECK_TRUE(shutdown(a));
                }
                configs[boundary_index + 1].m_chunk_size_shift = 14;
            }
        }

        UNITTEST_TEST(size_classes_and_caller_owned_input)
        {
            alloc_config_t configs[113];
            for (u32 i = 0; i < DARRAYSIZE(configs); ++i)
                configs[i] = {(u8)i, 31, 31, 11};

            for (u32 D = 2; D <= 4; D += 2)
            {
                const u32 count = D * 28 + 1;
                allocator_t* a = create_allocator((u64)1 << 24, 32768, configs, count, 8, 8, D);
                CHECK_TRUE(a != nullptr);
                if (a == nullptr)
                    continue;

                CHECK_TRUE(s_get_bin_config_for_size(a, 1) == &a->m_bin_config_array[0]);
                CHECK_TRUE(s_get_bin_config_for_size(a, 8) == &a->m_bin_config_array[0]);
                CHECK_TRUE(s_get_bin_config_for_size(a, 9) == &a->m_bin_config_array[D]);
                CHECK_TRUE(s_get_bin_config_for_size(a, 17) == &a->m_bin_config_array[D + D / 2]);
                CHECK_TRUE(s_get_bin_config_for_size(a, ((u32)1 << 31) - 1) == &a->m_bin_config_array[count - 1]);
                CHECK_TRUE(s_get_bin_config_for_size(a, (u32)1 << 31) == &a->m_bin_config_array[count - 1]);
                CHECK_TRUE(s_get_bin_config_for_size(a, ((u32)1 << 31) + 1) == nullptr);
                CHECK_EQUAL((u32)1 << 31, a->m_item_info_per_alloc_index[count - 1].m_item_size);
                CHECK_EQUAL((u32)1, a->m_item_info_per_alloc_index[count - 1].m_item_capacity);
                CHECK_TRUE(s_get_bin_config_for_size(a, 0xffffffffu) == nullptr);
                CHECK_TRUE(s_get_bin_config_for_size(a, 0) == nullptr);
                configs[0].m_item_cap_shift = 7;
                CHECK_EQUAL((u8)11, a->m_chunk_config_array[a->m_bin_config_array[0].m_chunk_cfg_index].m_item_cap_shift);
                configs[0].m_item_cap_shift = 11;
                CHECK_TRUE(shutdown(a));
            }

            allocator_t* a = create_allocator((u64)1 << 24, 32768, configs, 1, 8, 8, 4);
            CHECK_TRUE(a != nullptr);
            if (a != nullptr)
            {
                CHECK_TRUE(s_get_bin_config_for_size(a, 8) == &a->m_bin_config_array[0]);
                CHECK_TRUE(s_get_bin_config_for_size(a, 9) == nullptr);
                CHECK_TRUE(shutdown(a));
            }
        }

        UNITTEST_TEST(size_class_boundaries)
        {
            alloc_config_t configs[113];
            for (u32 i = 0; i < DARRAYSIZE(configs); ++i)
                configs[i] = {(u8)i, 31, 31, 11};

            for (u32 D = 2; D <= 4; D += 2)
            {
                allocator_t* a = create_allocator((u64)1 << 24, 32768, configs, D * 28 + 1, 8, 8, D);
                CHECK_TRUE(a != nullptr);
                if (a == nullptr)
                    continue;

                for (u32 i = 0; i < a->m_num_bin_configs; ++i)
                {
                    const u32 exponent = 3 + i / D;
                    const u64 base = (u64)1 << exponent;
                    const u64 boundary = base + (base / D) * (i % D);
                    CHECK_EQUAL((u32)((boundary + 7) & ~(u64)7), a->m_item_info_per_alloc_index[i].m_item_size);
                    for (s32 delta = -8; delta <= 8; ++delta)
                    {
                        const u64 candidate = boundary + delta;
                        if (candidate == 0 || candidate > 0xffffffffu)
                            continue;
                        const u32 request = (u32)candidate;
                        CHECK_TRUE(s_get_bin_config_for_size(a, request) == reference_bin(a, request));
                    }
                }
                CHECK_TRUE(s_get_bin_config_for_size(a, 0) == nullptr);
                CHECK_TRUE(s_get_bin_config_for_size(a, 0xffffffffu) == nullptr);
                CHECK_TRUE(shutdown(a));
            }
        }

        UNITTEST_TEST(configuration_commit_grows_for_large_lookup)
        {
            if (sizeof(uint_t) < sizeof(u64))
                return;

            const u64   page_size      = v_alloc_get_page_size();
            const u64   initial_commit = (sizeof(allocator_t) + 2048 + 32 * 32 + page_size - 1) & ~(page_size - 1);
            const u64   lookup_count   = initial_commit / sizeof(u32) + 1;
            allocator_t* a = initialize_test(lookup_count << 24, 32768);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;

            CHECK_TRUE((byte*)(a->m_o_region_lookup_array + lookup_count) > a->m_config_memory_base + initial_commit);
            CHECK_EQUAL((u32)0xffffffff, a->m_o_region_lookup_array[0]);
            CHECK_EQUAL((u32)0xffffffff, a->m_o_region_lookup_array[lookup_count - 1]);
#if defined(TARGET_MAC) || defined(TARGET_LINUX)
            const u64 lookup_end = (u64)((byte*)(a->m_o_region_lookup_array + lookup_count) - a->m_config_memory_base);
            const u64 committed = ((lookup_end > initial_commit ? lookup_end : initial_commit) + page_size - 1) & ~(page_size - 1);
            CHECK_TRUE(committed < a->m_config_memory_size);
            if (committed < a->m_config_memory_size)
                CHECK_TRUE(page_is_inaccessible(a->m_config_memory_base + committed));
#endif
            CHECK_TRUE(shutdown(a));
        }

#if defined(TARGET_MAC) || defined(TARGET_LINUX)
        UNITTEST_TEST(configuration_reserves_without_committing_unused_pages)
        {
            allocator_t* a = initialize_test((u64)1 << 24, 32768);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            const u64 page_size = v_alloc_get_page_size();
            const u64 scratch_end = sizeof(allocator_t) + 2048 + 32 * 32;
            const u64 lookup_end = (u64)((byte*)(a->m_o_region_lookup_array + 1) - a->m_config_memory_base);
            const u64 committed = ((scratch_end > lookup_end ? scratch_end : lookup_end) + page_size - 1) & ~(page_size - 1);
            CHECK_TRUE(committed < a->m_config_memory_size);
            if (committed < a->m_config_memory_size)
                CHECK_TRUE(page_is_inaccessible(a->m_config_memory_base + committed));
            CHECK_EQUAL((u32)0xffffffff, a->m_o_region_lookup_array[0]);
            CHECK_TRUE(shutdown(a));
        }
#endif

        UNITTEST_TEST(maximum_bins_with_distinct_configurations)
        {
            alloc_config_t configs[113];
            for (u32 i = 0; i < DARRAYSIZE(configs); ++i)
            {
                const u8 region_shift = i < 80 ? (u8)(24 + i / 10) : 31;
                const u8 chunk_shift = i < 80 ? (u8)(region_shift - 9 + i % 10) : 31;
                configs[i] = {(u8)i, region_shift, chunk_shift, (u8)(i / 80)};
            }

            allocator_t* a = create_allocator((u64)1 << 24, 32768, configs, DARRAYSIZE(configs), 8, 8, 4);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            CHECK_EQUAL((u16)80, a->m_num_region_configs);
            for (u32 i = 0; i < DARRAYSIZE(configs); ++i)
            {
                const bin_config_t& bin = a->m_bin_config_array[i];
                CHECK_EQUAL(configs[i].m_chunk_size_shift, a->m_chunk_config_array[bin.m_chunk_cfg_index].m_chunk_size_shift);
                CHECK_EQUAL(configs[i].m_item_cap_shift, a->m_chunk_config_array[bin.m_chunk_cfg_index].m_item_cap_shift);
                CHECK_EQUAL(configs[i].m_region_size_shift, a->m_region_config_array[bin.m_region_cfg_index].m_region_size_shift);
                CHECK_EQUAL(configs[i].m_chunk_size_shift, a->m_region_config_array[bin.m_region_cfg_index].m_chunk_size_shift);
            }
            CHECK_TRUE(shutdown(a));
        }

#if defined(TARGET_MAC) || defined(TARGET_LINUX)
        UNITTEST_TEST(shutdown_can_retry_failed_release)
        {
            allocator_t* a = initialize_test((u64)1 << 24, 32768);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;

            byte* managed_base = a->m_memory_base;
            a->m_memory_base = managed_base + 1;
            CHECK_FALSE(shutdown(a));
            CHECK_TRUE(a->m_arena_memory_base == nullptr);
            CHECK_TRUE(a->m_memory_base == managed_base + 1);
            a->m_memory_base = managed_base;

            const u64 config_size = a->m_config_memory_size;
            a->m_config_memory_size = 0;
            CHECK_FALSE(shutdown(a));
            CHECK_TRUE(a->m_memory_base == nullptr);
            CHECK_TRUE(a->m_arena_memory_base == nullptr);
            a->m_config_memory_size = config_size;
            CHECK_TRUE(shutdown(a));
        }
#endif

        UNITTEST_TEST(creates_regions_and_maps_managed_ranges)
        {
            allocator_t* a = initialize_test((u64)4 << 24, 32768);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;

            const u8 small_config = a->m_bin_config_array[0].m_region_cfg_index;
            const u8 large_config = a->m_bin_config_array[7].m_region_cfg_index;
            region_t* first = s_create_region(a, small_config);
            region_t* large = s_create_region(a, large_config);
            region_t* last = s_create_region(a, small_config);
            CHECK_TRUE(first != nullptr && large != nullptr && last != nullptr);
            if (first != nullptr && large != nullptr && last != nullptr)
            {
                const u32 first_offset = (u32)((byte*)first - a->m_arena_memory_base);
                const u32 large_offset = (u32)((byte*)large - a->m_arena_memory_base);
                const u32 last_offset = (u32)((byte*)last - a->m_arena_memory_base);
                CHECK_EQUAL((u32)0, first->m_index);
                CHECK_EQUAL((u32)1, large->m_index);
                CHECK_EQUAL((u32)3, last->m_index);
                CHECK_EQUAL((u32)4, a->m_next_region_index);
                CHECK_EQUAL(first_offset, a->m_o_region_lookup_array[0]);
                CHECK_EQUAL(large_offset, a->m_o_region_lookup_array[1]);
                CHECK_EQUAL(large_offset, a->m_o_region_lookup_array[2]);
                CHECK_EQUAL(last_offset, a->m_o_region_lookup_array[3]);
                CHECK_EQUAL(last_offset, a->m_active_o_region_per_region_index[small_config]);
                CHECK_EQUAL(first_offset, last->m_o_next);
                CHECK_EQUAL(large_offset, a->m_active_o_region_per_region_index[large_config]);
                CHECK_EQUAL((u32)0xffffffff, large->m_o_next);
                CHECK_EQUAL((u8)2, first->m_type);
                CHECK_EQUAL(small_config, first->m_region_cfg_index);
                CHECK_EQUAL((u16)1024, first->m_capacity);
                CHECK_EQUAL((u16)32, large->m_capacity);
                CHECK_EQUAL((u16)0, first->m_count);
                CHECK_EQUAL((u16)0, first->m_free_slot_index);
                CHECK_EQUAL((u16)0xffff, first->m_free_list);
                CHECK_TRUE(s_create_region(a, small_config) == nullptr);
                CHECK_EQUAL((u32)4, a->m_next_region_index);
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(active_region_list_push_pop)
        {
            allocator_t* a = initialize_test((u64)4 << 24, 32768);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            const u8 small_config = a->m_bin_config_array[0].m_region_cfg_index;
            const u8 large_config = a->m_bin_config_array[7].m_region_cfg_index;
            CHECK_TRUE(s_pop_active_region(a, small_config) == nullptr);
            region_t* first = s_create_region(a, small_config);
            region_t* other = first == nullptr ? nullptr : s_create_region(a, large_config);
            region_t* second = other == nullptr ? nullptr : s_create_region(a, small_config);
            CHECK_TRUE(second != nullptr);
            if (second != nullptr)
            {
                CHECK_TRUE(s_pop_active_region(a, small_config) == second);
                CHECK_TRUE(s_pop_active_region(a, large_config) == other);
                CHECK_TRUE(s_pop_active_region(a, large_config) == nullptr);
                CHECK_TRUE(s_pop_active_region(a, small_config) == first);
                CHECK_TRUE(s_pop_active_region(a, small_config) == nullptr);
                s_push_active_region(a, first);
                s_push_active_region(a, second);
                s_push_active_region(a, other);
                CHECK_TRUE(s_pop_active_region(a, small_config) == second);
                CHECK_TRUE(s_pop_active_region(a, small_config) == first);
                CHECK_TRUE(s_pop_active_region(a, small_config) == nullptr);
                CHECK_TRUE(s_pop_active_region(a, large_config) == other);
                CHECK_TRUE(s_pop_active_region(a, large_config) == nullptr);
                CHECK_EQUAL((u16)0, first->m_count);
                CHECK_EQUAL((u16)0, second->m_count);
                CHECK_EQUAL((u16)0, other->m_count);
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(region_slot_capacity_limits)
        {
            const alloc_config_t configs[] = {{0, 24, 24, 0}, {1, 25, 14, 0}};
            allocator_t* a = create_allocator((u64)3 << 24, 32768, configs, 2, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* single = s_create_region(a, a->m_bin_config_array[0].m_region_cfg_index);
            region_t* maximum = s_create_region(a, a->m_bin_config_array[1].m_region_cfg_index);
            CHECK_TRUE(single != nullptr && maximum != nullptr);
            if (single != nullptr && maximum != nullptr)
            {
                CHECK_EQUAL((u16)1, single->m_capacity);
                CHECK_EQUAL((u16)2048, maximum->m_capacity);
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(region_slot_pop_reports_exhaustion_and_reuses_slots)
        {
            const alloc_config_t config[] = {{0, 24, 23, 0}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            CHECK_TRUE(region != nullptr);
            if (region != nullptr)
            {
                u32* slots = (u32*)(a->m_arena_memory_base + region->m_o_array);
                slots[0] = 0x12345678;
                slots[1] = 0xabcdef01;
                CHECK_EQUAL((s32)0, s_pop_region_slot(a, region));
                CHECK_EQUAL((s32)1, s_pop_region_slot(a, region));
                CHECK_EQUAL((s32)-1, s_pop_region_slot(a, region));
                CHECK_EQUAL((s32)-1, s_pop_region_slot(a, region));
                CHECK_EQUAL((u16)2, region->m_free_slot_index);
                CHECK_EQUAL((u32)0x12345678, slots[0]);
                CHECK_EQUAL((u32)0xabcdef01, slots[1]);
                s_push_region_slot(a, region, 0);
                s_push_region_slot(a, region, 1);
                CHECK_EQUAL((s32)1, s_pop_region_slot(a, region));
                CHECK_EQUAL((s32)0, s_pop_region_slot(a, region));
                CHECK_EQUAL((s32)-1, s_pop_region_slot(a, region));
                CHECK_EQUAL((u16)2, region->m_free_slot_index);
                CHECK_EQUAL((u16)0xffff, region->m_free_list);
                CHECK_TRUE(s_create_block(a, region, 0) == nullptr);
                CHECK_EQUAL((u16)0, region->m_count);
                CHECK_EQUAL((u16)2, region->m_free_slot_index);
                CHECK_EQUAL((u32)0xffffffff, a->m_not_full_o_chunks_per_alloc_index[0]);
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(region_creation_leaves_virgin_slots_untouched)
        {
            allocator_t* a = initialize_test((u64)1 << 24, 32768);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;

            arena_pool_t* pool = &a->m_arenas[10];
            u32* slots = (u32*)s_allocate(pool);
            CHECK_TRUE(slots != nullptr);
            if (slots != nullptr)
            {
                for (u32 i = 0; i < 1024; ++i)
                    slots[i] = i;
                s_free(pool, slots);

                region_t* region = s_create_region(a, a->m_bin_config_array[0].m_region_cfg_index);
                CHECK_TRUE(region != nullptr);
                if (region != nullptr)
                {
                    u32* region_slots = (u32*)(a->m_arena_memory_base + region->m_o_array);
                    CHECK_TRUE(region_slots == slots);
                    CHECK_EQUAL((u16)0, region->m_free_slot_index);
                    CHECK_EQUAL((u32)0xffffffff, region_slots[0]);
                    for (u32 i = 1; i < region->m_capacity; ++i)
                        CHECK_EQUAL(i, region_slots[i]);
                }
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(region_creation_rolls_back_on_metadata_exhaustion)
        {
            allocator_t* a = initialize_test((u64)1 << 24, 32768);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            const u8 config = a->m_bin_config_array[0].m_region_cfg_index;
            arena_pool_t& array_pool = a->m_arenas[10];
            const u32 reserved_pages = array_pool.m_reserved_pages;
            array_pool.m_reserved_pages = 0;
            CHECK_TRUE(s_create_region(a, config) == nullptr);
            CHECK_EQUAL((u32)0, a->m_next_region_index);
            CHECK_EQUAL((u32)0xffffffff, a->m_o_region_lookup_array[0]);
            CHECK_EQUAL((u32)0xffffffff, a->m_active_o_region_per_region_index[config]);
            array_pool.m_reserved_pages = reserved_pages;
            region_t* region = s_create_region(a, config);
            CHECK_TRUE(region != nullptr);
            if (region != nullptr)
                CHECK_TRUE((byte*)region == a->m_arenas[0].m_memory_base);
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(creates_chunks_and_blocks_in_active_region)
        {
            const alloc_config_t configs[] = {{0, 24, 23, 7}, {1, 24, 23, 6}};
            allocator_t* a = create_allocator((u64)2 << 24, 32768, configs, 2, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;

            const u8 config = a->m_bin_config_array[0].m_region_cfg_index;
            region_t* older = s_create_region(a, config);
            region_t* region = s_create_region(a, config);
            CHECK_TRUE(older != nullptr && region != nullptr);
            if (older != nullptr && region != nullptr)
            {
                arena_pool_t* bin_pool = &a->m_arenas[2];
                u64* recycled = (u64*)s_allocate(bin_pool);
                CHECK_TRUE(recycled != nullptr);
                if (recycled != nullptr)
                {
                    recycled[1] = 0x123456789abcdef0ull;
                    s_free(bin_pool, recycled);
                    chunk_t* chunk = s_create_chunk(a, region, 0);
                    CHECK_TRUE(chunk != nullptr);
                    if (chunk != nullptr)
                    {
                        const u32 chunk_offset = (u32)((byte*)chunk - a->m_arena_memory_base);
                        const u32 region_offset = (u32)((byte*)region - a->m_arena_memory_base);
                        u32* slots = (u32*)(a->m_arena_memory_base + region->m_o_array);
                        CHECK_EQUAL((u8)0, chunk->m_type);
                        chunk2_t* info = chunk_info(a, chunk);
                        CHECK_EQUAL((u8)0, chunk->m_bin_index);
                        CHECK_EQUAL((u16)0, chunk->m_index);
                        CHECK_EQUAL((u16)0, info->m_item_count);
                        CHECK_EQUAL((u16)0, info->m_item_free_index);
                        CHECK_EQUAL((u16)8, info->m_item_size);
                        CHECK_EQUAL((u16)128, info->m_item_capacity);
                        CHECK_EQUAL((u64)0, info->m_bin0);
                        CHECK_TRUE(a->m_arena_memory_base + info->m_o_bin1 == (byte*)recycled);
                        CHECK_EQUAL((u64)0x123456789abcdef0ull, recycled[1]);
                        CHECK_EQUAL((u32)0xffffffff, chunk->m_o_tags);
                        CHECK_EQUAL(region_offset, chunk->m_o_region);
                        CHECK_EQUAL((u32)0xffffffff, chunk->m_o_next);
                        CHECK_EQUAL(chunk_offset, slots[0]);
                        CHECK_EQUAL(chunk_offset, a->m_not_full_o_chunks_per_alloc_index[0]);
                        CHECK_EQUAL((u16)1, region->m_count);
                        CHECK_EQUAL((u16)1, region->m_free_slot_index);

                        block_t* block = s_create_block(a, region, 1);
                        CHECK_TRUE(block != nullptr);
                        if (block != nullptr)
                        {
                            CHECK_EQUAL((u8)1, block->m_type);
                            CHECK_EQUAL((u8)1, block->m_bin_index);
                            CHECK_EQUAL((u16)1, block->m_index);
                            CHECK_EQUAL((u32)0, block->m_pages);
                            CHECK_EQUAL(region_offset, block->m_o_region);
                            CHECK_EQUAL((u32)((byte*)block - a->m_arena_memory_base), slots[1]);
                            CHECK_EQUAL((u16)2, region->m_count);
                            CHECK_EQUAL((u32)((byte*)older - a->m_arena_memory_base), a->m_active_o_region_per_region_index[config]);
                            CHECK_TRUE(s_create_block(a, region, 1) == nullptr);
                        }
                    }
                }

                chunk_t* small = s_create_chunk(a, older, 1);
                CHECK_TRUE(small != nullptr);
                if (small != nullptr)
                    CHECK_EQUAL((u32)0xffffffff, chunk_info(a, small)->m_o_bin1);
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(chunk_creation_rolls_back_when_bitmap_pool_is_full)
        {
            const alloc_config_t config[] = {{0, 24, 23, 7}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            CHECK_TRUE(region != nullptr);
            if (region != nullptr)
            {
                arena_pool_t& pool = a->m_arenas[2];
                const u32 reserved = pool.m_reserved_pages;
                pool.m_reserved_pages = 0;
                CHECK_TRUE(s_create_chunk(a, region, 0) == nullptr);
                CHECK_EQUAL((u16)0, region->m_count);
                CHECK_EQUAL((u16)0, region->m_free_slot_index);
                CHECK_EQUAL((u32)0xffffffff, a->m_not_full_o_chunks_per_alloc_index[0]);
                byte* cursor = a->m_arenas[0].m_memory_cursor;
                pool.m_reserved_pages = reserved;
                chunk_t* chunk = s_create_chunk(a, region, 0);
                CHECK_TRUE(chunk != nullptr);
                if (chunk != nullptr)
                {
                    CHECK_TRUE((byte*)chunk == a->m_arenas[0].m_memory_base + a->m_arenas[0].m_allocation_size);
                    CHECK_TRUE((byte*)chunk_info(a, chunk) == (byte*)chunk + a->m_arenas[0].m_allocation_size);
                    CHECK_TRUE(a->m_arenas[0].m_memory_cursor == cursor);
                }
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(chunk_creation_rolls_back_when_second_metadata_slot_is_unavailable)
        {
            const alloc_config_t config[] = {{0, 24, 23, 6}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            CHECK_TRUE(region != nullptr);
            if (region != nullptr)
            {
                arena_pool_t& pool = a->m_arenas[0];
                void* slot = s_allocate(&pool);
                CHECK_TRUE(slot != nullptr);
                if (slot != nullptr)
                {
                    s_free(&pool, slot);
                    const u32 reserved = pool.m_reserved_pages;
                    pool.m_reserved_pages = 0;
                    CHECK_TRUE(s_create_chunk(a, region, 0) == nullptr);
                    CHECK_EQUAL((u16)0, region->m_count);
                    CHECK_EQUAL((u16)0, region->m_free_slot_index);
                    CHECK_EQUAL((u32)0xffffffff, a->m_not_full_o_chunks_per_alloc_index[0]);
                    CHECK_EQUAL((u32)((byte*)slot - pool.m_memory_base), pool.m_free_list);
                    pool.m_reserved_pages = reserved;
                    chunk_t* chunk = s_create_chunk(a, region, 0);
                    CHECK_TRUE((void*)chunk == slot);
                    if (chunk != nullptr)
                        CHECK_TRUE((byte*)chunk_info(a, chunk) == (byte*)slot + pool.m_allocation_size);
                }
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(creation_reuses_a_freed_region_slot)
        {
            const alloc_config_t config[] = {{0, 24, 23, 6}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            CHECK_TRUE(region != nullptr);
            if (region != nullptr)
            {
                u32* slots = (u32*)(a->m_arena_memory_base + region->m_o_array);
                region->m_free_slot_index = 1;
                region->m_free_list = 0;
                slots[0] = 0xffffu;
                block_t* block = s_create_block(a, region, 0);
                CHECK_TRUE(block != nullptr);
                if (block != nullptr)
                {
                    CHECK_EQUAL((u16)0, block->m_index);
                    CHECK_EQUAL((u16)1, region->m_free_slot_index);
                    CHECK_EQUAL((u16)0xffff, region->m_free_list);
                    CHECK_EQUAL((u16)1, region->m_count);
                    CHECK_EQUAL((u32)((byte*)block - a->m_arena_memory_base), slots[0]);
                }
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(freeing_blocks_reactivates_full_region_and_reuses_slots)
        {
            const alloc_config_t config[] = {{0, 24, 23, 6}};
            allocator_t* a = create_allocator((u64)2 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* older = s_create_region(a, 0);
            region_t* region = s_create_region(a, 0);
            CHECK_TRUE(older != nullptr && region != nullptr);
            if (older != nullptr && region != nullptr)
            {
                block_t* first = s_create_block(a, region, 0);
                block_t* second = s_create_block(a, region, 0);
                CHECK_TRUE(first != nullptr && second != nullptr);
                if (first != nullptr && second != nullptr)
                {
                    u32* slots = (u32*)(a->m_arena_memory_base + region->m_o_array);
                    const u32 region_offset = (u32)((byte*)region - a->m_arena_memory_base);
                    const u32 older_offset = (u32)((byte*)older - a->m_arena_memory_base);
                    CHECK_EQUAL(older_offset, a->m_active_o_region_per_region_index[0]);
                    CHECK_TRUE(s_free_block(a, first));
                    CHECK_EQUAL((u16)1, region->m_count);
                    CHECK_EQUAL((u16)0, region->m_free_list);
                    CHECK_EQUAL((u32)0xffff, slots[0]);
                    CHECK_EQUAL(region_offset, a->m_active_o_region_per_region_index[0]);
                    CHECK_EQUAL(older_offset, region->m_o_next);
                    CHECK_TRUE(s_free_block(a, second));
                    CHECK_EQUAL((u16)0, region->m_count);
                    CHECK_EQUAL((u16)1, region->m_free_list);
                    CHECK_EQUAL((u32)0, slots[1]);
                    CHECK_EQUAL(region_offset, a->m_active_o_region_per_region_index[0]);
                    CHECK_EQUAL(older_offset, region->m_o_next);
                    CHECK_EQUAL(region_offset, a->m_o_region_lookup_array[1]);

                    block_t* reused_second = s_create_block(a, region, 0);
                    block_t* reused_first = s_create_block(a, region, 0);
                    CHECK_TRUE(reused_second == second && reused_first == first);
                    if (reused_second != nullptr && reused_first != nullptr)
                    {
                        CHECK_EQUAL((u16)1, reused_second->m_index);
                        CHECK_EQUAL((u16)0, reused_first->m_index);
                        CHECK_EQUAL((u16)2, region->m_free_slot_index);
                        CHECK_EQUAL((u16)0xffff, region->m_free_list);
                        CHECK_EQUAL((u16)2, region->m_count);
                        CHECK_EQUAL(older_offset, a->m_active_o_region_per_region_index[0]);
                    }
                }
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(freeing_block_decommits_only_its_committed_pages)
        {
            const alloc_config_t config[] = {{0, 24, 16, 6}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            block_t* first = region == nullptr ? nullptr : s_create_block(a, region, 0);
            block_t* second = first == nullptr ? nullptr : s_create_block(a, region, 0);
            CHECK_TRUE(first != nullptr && second != nullptr);
            if (first != nullptr && second != nullptr)
            {
                const u32 page_size = v_alloc_get_page_size();
                byte* first_address = a->m_memory_base;
                byte* second_address = a->m_memory_base + ((u32)1 << 16);
                const bool first_committed = v_alloc_commit(first_address, page_size);
                const bool second_committed = v_alloc_commit(second_address, page_size * 2);
                const bool trailing_committed = v_alloc_commit(second_address + page_size * 2, page_size);
                CHECK_TRUE(first_committed && second_committed && trailing_committed);
                if (first_committed && second_committed && trailing_committed)
                {
                    first->m_pages = 1;
                    second->m_pages = 2;
                    first_address[0] = 0x41;
                    second_address[0] = 0x42;
                    second_address[page_size * 2 - 1] = 0x43;
                    second_address[page_size * 2] = 0x46;
                    CHECK_TRUE(s_free_block(a, second));
                    CHECK_EQUAL((u8)0x41, first_address[0]);
                    CHECK_EQUAL((u16)1, region->m_count);
                    CHECK_EQUAL((u16)1, region->m_free_list);
                    CHECK_EQUAL((u32)((byte*)region - a->m_arena_memory_base), a->m_active_o_region_per_region_index[0]);
#if defined(TARGET_MAC) || defined(TARGET_LINUX)
                    CHECK_TRUE(page_is_inaccessible(second_address));
                    CHECK_TRUE(page_is_inaccessible(second_address + page_size));
                    const bool trailing_accessible = !page_is_inaccessible(second_address + page_size * 2);
                    CHECK_TRUE(trailing_accessible);
                    if (trailing_accessible)
#endif
                        CHECK_EQUAL((u8)0x46, second_address[page_size * 2]);
                    const bool recommitted = v_alloc_commit(second_address, page_size * 2);
                    CHECK_TRUE(recommitted);
                    if (recommitted)
                    {
                        second_address[0] = 0x44;
                        second_address[page_size * 2 - 1] = 0x45;
                        CHECK_TRUE(v_alloc_decommit(second_address, page_size * 2));
                    }
                    CHECK_TRUE(v_alloc_decommit(second_address + page_size * 2, page_size));
                    CHECK_TRUE(s_free_block(a, first));
                }
            }
            CHECK_TRUE(shutdown(a));
        }

#if defined(TARGET_MAC) || defined(TARGET_LINUX)
        UNITTEST_TEST(block_decommit_failure_preserves_region_and_block)
        {
            const alloc_config_t config[] = {{0, 24, 24, 6}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            block_t* block = region == nullptr ? nullptr : s_create_block(a, region, 0);
            CHECK_TRUE(block != nullptr);
            if (block != nullptr)
            {
                byte* base = a->m_memory_base;
                const bool committed = v_alloc_commit(base, v_alloc_get_page_size());
                CHECK_TRUE(committed);
                if (committed)
                {
                    block->m_pages = 1;
                    u32* slots = (u32*)(a->m_arena_memory_base + region->m_o_array);
                    const u32 block_offset = (u32)((byte*)block - a->m_arena_memory_base);
                    a->m_memory_base = base + 1;
                    CHECK_FALSE(s_free_block(a, block));
                    a->m_memory_base = base;
                    CHECK_EQUAL((u32)1, block->m_pages);
                    CHECK_EQUAL((u16)1, region->m_count);
                    CHECK_EQUAL((u16)0xffff, region->m_free_list);
                    CHECK_EQUAL(block_offset, slots[0]);
                    CHECK_EQUAL((u32)0xffffffff, a->m_active_o_region_per_region_index[0]);
                    CHECK_TRUE(s_free_block(a, block));
                    CHECK_EQUAL((u16)0, region->m_count);
                    CHECK_EQUAL((u32)((byte*)region - a->m_arena_memory_base), a->m_active_o_region_per_region_index[0]);
                }
            }
            CHECK_TRUE(shutdown(a));
        }
#endif

        UNITTEST_TEST(chunk_list_and_metadata_exhaustion)
        {
            const alloc_config_t config[] = {{0, 24, 22, 12}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            CHECK_TRUE(region != nullptr);
            if (region != nullptr)
            {
                arena_pool_t& metadata = a->m_arenas[0];
                const u32 reserved = metadata.m_reserved_pages;
                metadata.m_reserved_pages = 0;
                CHECK_TRUE(s_create_chunk(a, region, 0) == nullptr);
                CHECK_TRUE(s_create_block(a, region, 0) == nullptr);
                CHECK_EQUAL((u16)0, region->m_count);
                CHECK_EQUAL((u16)0, region->m_free_slot_index);
                metadata.m_reserved_pages = reserved;

                chunk_t* first = s_create_chunk(a, region, 0);
                chunk_t* second = s_create_chunk(a, region, 0);
                CHECK_TRUE(first != nullptr && second != nullptr);
                if (first != nullptr && second != nullptr)
                {
                    CHECK_EQUAL((u32)((byte*)first - a->m_arena_memory_base), second->m_o_next);
                    CHECK_EQUAL((u32)((byte*)second - a->m_arena_memory_base), a->m_not_full_o_chunks_per_alloc_index[0]);
                    CHECK_TRUE(a->m_arena_memory_base + chunk_info(a, first)->m_o_bin1 == a->m_arenas[7].m_memory_base);
                    CHECK_TRUE(a->m_arena_memory_base + chunk_info(a, second)->m_o_bin1 == a->m_arenas[7].m_memory_base + 512);
                }
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(not_full_chunk_list_push_pop)
        {
            const alloc_config_t configs[] = {{0, 24, 14, 1}, {1, 24, 14, 1}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, configs, 2, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            CHECK_TRUE(s_pop_not_full_chunk(a, 0) == nullptr);
            region_t* region = s_create_region(a, 0);
            chunk_t* first = region == nullptr ? nullptr : s_create_chunk(a, region, 0);
            chunk_t* second = first == nullptr ? nullptr : s_create_chunk(a, region, 0);
            chunk_t* other = second == nullptr ? nullptr : s_create_chunk(a, region, 1);
            CHECK_TRUE(other != nullptr);
            if (other != nullptr)
            {
                CHECK_TRUE(s_pop_not_full_chunk(a, 0) == second);
                CHECK_TRUE(s_pop_not_full_chunk(a, 1) == other);
                CHECK_TRUE(s_pop_not_full_chunk(a, 1) == nullptr);
                CHECK_TRUE(s_pop_not_full_chunk(a, 0) == first);
                CHECK_TRUE(s_pop_not_full_chunk(a, 0) == nullptr);
                s_push_not_full_chunk(a, first);
                s_push_not_full_chunk(a, second);
                s_push_not_full_chunk(a, other);
                CHECK_TRUE(s_pop_not_full_chunk(a, 0) == second);
                CHECK_TRUE(s_pop_not_full_chunk(a, 0) == first);
                CHECK_TRUE(s_pop_not_full_chunk(a, 0) == nullptr);
                CHECK_TRUE(s_pop_not_full_chunk(a, 1) == other);
                CHECK_TRUE(s_pop_not_full_chunk(a, 1) == nullptr);
                CHECK_EQUAL((u16)3, region->m_count);
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(allocates_chunk_items_and_reuses_bitmap_entries)
        {
            const alloc_config_t configs[] = {{0, 24, 14, 10}, {1, 24, 14, 10}, {2, 24, 14, 10}, {3, 24, 14, 10}};
            allocator_t* a = create_allocator((u64)2 << 24, 32768, configs, 4, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* older = s_create_region(a, 0);
            region_t* region = s_create_region(a, 0);
            CHECK_TRUE(older != nullptr && region != nullptr);
            if (older != nullptr && region != nullptr)
            {
                chunk_t* first = s_create_chunk(a, region, 3);
                chunk_t* chunk = s_create_chunk(a, region, 3);
                CHECK_TRUE(first != nullptr && chunk != nullptr);
                if (first != nullptr && chunk != nullptr)
                {
                    chunk2_t* info = chunk_info(a, chunk);
                    CHECK_EQUAL((u32)24, a->m_item_info_per_alloc_index[3].m_item_size);
                    CHECK_EQUAL((u32)682, a->m_item_info_per_alloc_index[3].m_item_capacity);
                    CHECK_EQUAL((u16)24, info->m_item_size);
                    CHECK_EQUAL((u16)682, info->m_item_capacity);
                    byte* expected = a->m_memory_base + ((u64)1 << 24) + ((u64)1 << 14);
                    for (u32 i = 0; i < 67; ++i)
                    {
                        byte* item = (byte*)s_allocate_chunk_item(a, chunk);
                        CHECK_TRUE(item == expected + i * 24);
                        if (item != nullptr)
                        {
                            item[0] = (byte)i;
                            item[23] = (byte)(i + 1);
                        }
                    }
                    s_free_chunk_item(a, chunk, expected + 64 * 24);
                    CHECK_TRUE(s_allocate_chunk_item(a, chunk) == expected + 64 * 24);
                    CHECK_EQUAL((u16)67, info->m_item_free_index);
                    for (u32 i = 67; i < 682; ++i)
                    {
                        byte* item = (byte*)s_allocate_chunk_item(a, chunk);
                        CHECK_TRUE(item == expected + i * 24);
                        if (item != nullptr)
                        {
                            item[0] = (byte)i;
                            item[23] = (byte)(i + 1);
                        }
                    }
                    CHECK_EQUAL((u8)1, expected[23]);
                    CHECK_EQUAL((u8)67, expected[66 * 24 + 23]);
                    CHECK_EQUAL((u8)(682 & 0xff), expected[681 * 24 + 23]);
                    CHECK_EQUAL((u16)682, info->m_item_count);
                    CHECK_TRUE(s_allocate_chunk_item(a, chunk) == nullptr);
                    CHECK_EQUAL((u32)((byte*)first - a->m_arena_memory_base), a->m_not_full_o_chunks_per_alloc_index[3]);
                    s_free_chunk_item(a, chunk, expected + 681 * 24);
                    s_free_chunk_item(a, chunk, expected + 64 * 24);
                    CHECK_EQUAL((u16)680, info->m_item_count);
                    CHECK_EQUAL((u16)682, info->m_item_free_index);
                    CHECK_EQUAL((u32)((byte*)chunk - a->m_arena_memory_base), a->m_not_full_o_chunks_per_alloc_index[3]);
                    CHECK_EQUAL((u32)((byte*)first - a->m_arena_memory_base), chunk->m_o_next);
                    CHECK_TRUE(s_allocate_chunk_item(a, chunk) == expected + 64 * 24);
                    CHECK_TRUE(s_allocate_chunk_item(a, chunk) == expected + 681 * 24);
                    CHECK_EQUAL((u16)682, info->m_item_count);
                    CHECK_EQUAL((u32)((byte*)first - a->m_arena_memory_base), a->m_not_full_o_chunks_per_alloc_index[3]);
                }
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(allocates_and_reuses_single_level_chunk_items)
        {
            const alloc_config_t config[] = {{0, 24, 14, 1}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            chunk_t* chunk = region == nullptr ? nullptr : s_create_chunk(a, region, 0);
            CHECK_TRUE(chunk != nullptr);
            if (chunk != nullptr)
            {
                chunk2_t* info = chunk_info(a, chunk);
                byte* first = (byte*)s_allocate_chunk_item(a, chunk);
                CHECK_TRUE(first == a->m_memory_base);
                byte* second = (byte*)s_allocate_chunk_item(a, chunk);
                CHECK_TRUE(second == a->m_memory_base + 8);
                if (first != nullptr && second != nullptr)
                {
                    first[0] = 42;
                    second[7] = 43;
                }
                CHECK_EQUAL((u16)2, info->m_item_count);
                CHECK_EQUAL((u32)0xffffffff, a->m_not_full_o_chunks_per_alloc_index[0]);
                CHECK_TRUE(s_allocate_chunk_item(a, chunk) == nullptr);
                if (first != nullptr && second != nullptr)
                {
                    s_free_chunk_item(a, chunk, first);
                    s_free_chunk_item(a, chunk, second);
                    CHECK_EQUAL((u16)0, info->m_item_count);
                    CHECK_EQUAL((u16)2, info->m_item_free_index);
                    CHECK_EQUAL((u32)((byte*)chunk - a->m_arena_memory_base), a->m_not_full_o_chunks_per_alloc_index[0]);
                    CHECK_EQUAL((u32)0xffffffff, chunk->m_o_next);
                    CHECK_EQUAL((u8)42, first[0]);
                    CHECK_EQUAL((u8)43, second[7]);
                    CHECK_TRUE(s_allocate_chunk_item(a, chunk) == first);
                    CHECK_TRUE(s_allocate_chunk_item(a, chunk) == second);
                    CHECK_EQUAL((u32)0xffffffff, a->m_not_full_o_chunks_per_alloc_index[0]);
                }
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(freeing_nonfull_chunk_preserves_list_order)
        {
            const alloc_config_t config[] = {{0, 24, 14, 1}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            chunk_t* first = region == nullptr ? nullptr : s_create_chunk(a, region, 0);
            byte* item = first == nullptr ? nullptr : (byte*)s_allocate_chunk_item(a, first);
            chunk_t* second = item == nullptr ? nullptr : s_create_chunk(a, region, 0);
            CHECK_TRUE(second != nullptr);
            if (second != nullptr)
            {
                s_free_chunk_item(a, first, item);
                CHECK_EQUAL((u16)0, chunk_info(a, first)->m_item_count);
                CHECK_EQUAL((u32)((byte*)second - a->m_arena_memory_base), a->m_not_full_o_chunks_per_alloc_index[0]);
                CHECK_EQUAL((u32)((byte*)first - a->m_arena_memory_base), second->m_o_next);
                CHECK_EQUAL((u32)0xffffffff, first->m_o_next);
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(chunk_creation_commits_every_page_before_item_allocation)
        {
            const alloc_config_t config[] = {{0, 24, 16, 1}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, config, 1, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            chunk_t* first = region == nullptr ? nullptr : s_create_chunk(a, region, 0);
            CHECK_TRUE(first != nullptr);
            if (first != nullptr)
            {
                const u32 chunk_size = (u32)1 << 16;
                a->m_memory_base[chunk_size - 1] = 0xa5;
                u32* slots = (u32*)(a->m_arena_memory_base + region->m_o_array);
                region->m_free_slot_index = 2;
                region->m_free_list = 1;
                slots[1] = 0xffffu;
                chunk_t* chunk = s_create_chunk(a, region, 0);
                CHECK_TRUE(chunk != nullptr);
                if (chunk != nullptr)
                {
                    CHECK_EQUAL((u16)1, chunk->m_index);
                    CHECK_EQUAL((u16)2, region->m_free_slot_index);
                    byte* memory = a->m_memory_base + chunk_size;
                    const u32 page_size = v_alloc_get_page_size();
                    for (u32 i = 0; i < chunk_size; i += page_size)
                        memory[i] = (byte)(i / page_size + 1);
                    memory[chunk_size - 1] = 0x5a;
                    CHECK_EQUAL((u8)0xa5, a->m_memory_base[chunk_size - 1]);
                    CHECK_EQUAL((u8)0x5a, memory[chunk_size - 1]);
                    CHECK_TRUE(s_allocate_chunk_item(a, chunk) == memory);
                    CHECK_EQUAL((u8)1, memory[0]);
                }
            }
            CHECK_TRUE(shutdown(a));
        }

#if defined(TARGET_MAC) || defined(TARGET_LINUX)
        UNITTEST_TEST(chunk_creation_commit_failure_rolls_back)
        {
            allocator_t* a = initialize_test((u64)1 << 24, 32768);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, a->m_bin_config_array[0].m_region_cfg_index);
            CHECK_TRUE(region != nullptr);
            if (region != nullptr)
            {
                u32* slots = (u32*)(a->m_arena_memory_base + region->m_o_array);
                slots[0] = 0x12345678;
                byte* managed_base = a->m_memory_base;
                a->m_memory_base = managed_base + 1;
                CHECK_TRUE(s_create_chunk(a, region, 0) == nullptr);
                a->m_memory_base = managed_base;
                CHECK_EQUAL((u16)0, region->m_count);
                CHECK_EQUAL((u16)0, region->m_free_slot_index);
                CHECK_EQUAL((u16)0xffff, region->m_free_list);
                CHECK_EQUAL((u32)0x12345678, slots[0]);
                CHECK_EQUAL((u32)0xffffffff, a->m_not_full_o_chunks_per_alloc_index[0]);
                CHECK_EQUAL((u32)((byte*)region - a->m_arena_memory_base), a->m_active_o_region_per_region_index[region->m_region_cfg_index]);
                byte* cursor = a->m_arenas[0].m_memory_cursor;
                chunk_t* chunk = s_create_chunk(a, region, 0);
                CHECK_TRUE(chunk != nullptr);
                if (chunk != nullptr)
                {
                    CHECK_TRUE((byte*)chunk == a->m_arenas[0].m_memory_base + a->m_arenas[0].m_allocation_size);
                    CHECK_TRUE((byte*)chunk_info(a, chunk) == (byte*)chunk + a->m_arenas[0].m_allocation_size);
                    CHECK_TRUE(a->m_arenas[0].m_memory_cursor == cursor);
                    CHECK_TRUE(a->m_arena_memory_base + chunk_info(a, chunk)->m_o_bin1 == a->m_arenas[6].m_memory_base);
                    CHECK_TRUE(s_allocate_chunk_item(a, chunk) == managed_base);
                    block_t* block = s_create_block(a, region, 0);
                    CHECK_TRUE(block != nullptr);
                    if (block != nullptr)
                    {
                        CHECK_TRUE(s_free_block(a, block));
                        CHECK_EQUAL((u16)1, region->m_free_list);
                        a->m_memory_base = managed_base + 1;
                        CHECK_TRUE(s_create_chunk(a, region, 0) == nullptr);
                        a->m_memory_base = managed_base;
                        CHECK_EQUAL((u16)1, region->m_count);
                        CHECK_EQUAL((u16)2, region->m_free_slot_index);
                        CHECK_EQUAL((u16)1, region->m_free_list);
                        CHECK_EQUAL((u32)0xffff, slots[1]);
                        chunk_t* retry = s_create_chunk(a, region, 0);
                        CHECK_TRUE(retry != nullptr);
                        if (retry != nullptr)
                            CHECK_EQUAL((u16)1, retry->m_index);
                    }
                }
            }
            CHECK_TRUE(shutdown(a));
        }
#endif

        UNITTEST_TEST(chunk_item_stride_preserves_alignment)
        {
            const alloc_config_t configs[] = {{0, 24, 14, 12}, {1, 24, 14, 12}};
            allocator_t* a = create_allocator((u64)1 << 24, 32768, configs, 2, 8, 8, 2);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;
            region_t* region = s_create_region(a, 0);
            chunk_t* chunk = region == nullptr ? nullptr : s_create_chunk(a, region, 1);
            CHECK_TRUE(chunk != nullptr);
            if (chunk != nullptr)
            {
                CHECK_EQUAL((u32)16, a->m_item_info_per_alloc_index[1].m_item_size);
                CHECK_EQUAL((u32)1024, a->m_item_info_per_alloc_index[1].m_item_capacity);
                CHECK_TRUE(s_allocate_chunk_item(a, chunk) == a->m_memory_base);
                CHECK_TRUE(s_allocate_chunk_item(a, chunk) == a->m_memory_base + 16);
            }
            CHECK_TRUE(shutdown(a));
        }

        UNITTEST_TEST(pool_commits_grows_reuses_and_stops_at_capacity)
        {
            const u32 pool_size = 32768;
            allocator_t* a = initialize_test((u64)1 << 24, pool_size);
            CHECK_TRUE(a != nullptr);
            if (a == nullptr)
                return;

            arena_pool_t* pool = &a->m_arenas[0];
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
            CHECK_TRUE(pool->m_memory_cursor == pool->m_memory_base + count * pool->m_allocation_size);
            if (first != nullptr)
            {
                s_free(pool, first);
                CHECK_TRUE(s_allocate(pool) == first);
                CHECK_TRUE(s_allocate(pool) == nullptr);
            }
            arena_pool_t* large_pool = &a->m_arenas[11];
            byte* large = (byte*)s_allocate(large_pool);
            CHECK_TRUE(large != nullptr);
            if (large != nullptr)
            {
                large[0] = 1;
                large[8191] = 2;
                CHECK_EQUAL((u32)((8192 + page_size - 1) / page_size), large_pool->m_committed_pages);
            }
            CHECK_TRUE(shutdown(a));
        }
    }
}
UNITTEST_SUITE_END
