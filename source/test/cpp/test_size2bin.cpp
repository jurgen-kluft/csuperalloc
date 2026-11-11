#include "cbase/c_allocator.h"
#include "cbase/c_integer.h"
#include "ccore/c_random.h"

#include "csuperalloc/c_fsa.h"

#include "cunittest/cunittest.h"

using namespace ncore;

UNITTEST_SUITE_BEGIN(size2bin)
{
    static s32 s_size_to_bin_d2(u32 & alloc_size)
    {
        const s32 w   = math::countLeadingZeros(alloc_size);
        const u32 f   = (u32)0x80000000 >> w;
        const u32 r   = 0xFFFFFFFF << (30 - w);
        const u32 t   = ((f - 1) >> 1);
        alloc_size    = (alloc_size + t) & ~t;
        const s32 bin = (s32)((alloc_size & r) >> (30 - w)) + ((30 - w) * 2);
        return bin;
    }

    static s32 s_size_to_bin_d4(u32 & alloc_size)
    {
        const s32 w   = math::countLeadingZeros(alloc_size);
        const u32 f   = (u32)0x80000000 >> w;
        const u32 r   = 0xFFFFFFFF << (29 - w);
        const u32 t   = ((f - 1) >> 2);
        alloc_size    = (alloc_size + t) & ~t;
        const s32 bin = (s32)((alloc_size & r) >> (29 - w)) + ((29 - w) * 4);
        return bin;
    }

    UNITTEST_FIXTURE(main)
    {
        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(size_to_bin_d2_rounds_to_half_power_of_two_steps)
        {
            struct test_case_t
            {
                u32 m_request_size;
                u32 m_alloc_size;
                s32 m_bin;
            };

            test_case_t const test_cases[] = {
                {8, 8, 6},
                {9, 12, 7},
                {12, 12, 7},
                {13, 16, 8},
                {16, 16, 8},
                {17, 24, 9},
                {24, 24, 9},
                {25, 32, 10},
                {32, 32, 10},
            };

            for (u32 index = 0; index < sizeof(test_cases) / sizeof(test_cases[0]); ++index)
            {
                u32 const request_size = test_cases[index].m_request_size;
                u32       alloc_size   = request_size;
                s32 const bin          = s_size_to_bin_d2(alloc_size);

                CHECK_EQUAL(test_cases[index].m_alloc_size, alloc_size);
                CHECK_EQUAL(test_cases[index].m_bin, bin);
                CHECK_TRUE(alloc_size >= request_size);
            }
        }

        UNITTEST_TEST(size_to_bin_d4_rounds_to_quarter_power_of_two_steps)
        {
            struct test_case_t
            {
                u32 m_request_size;
                u32 m_alloc_size;
                s32 m_bin;
            };

            test_case_t const test_cases[] = {
                {8, 8, 8},
                {9, 10, 9},
                {10, 10, 9},
                {11, 12, 10},
                {12, 12, 10},
                {13, 14, 11},
                {14, 14, 11},
                {15, 16, 12},
                {16, 16, 12},
                {17, 20, 13},
                {20, 20, 13},
                {21, 24, 14},
                {24, 24, 14},
                {25, 28, 15},
                {28, 28, 15},
                {29, 32, 16},
                {32, 32, 16},
            };

            for (u32 index = 0; index < sizeof(test_cases) / sizeof(test_cases[0]); ++index)
            {
                u32 const request_size = test_cases[index].m_request_size;
                u32       alloc_size   = request_size;
                s32 const bin          = s_size_to_bin_d4(alloc_size);

                CHECK_EQUAL(test_cases[index].m_alloc_size, alloc_size);
                CHECK_EQUAL(test_cases[index].m_bin, bin);
                CHECK_TRUE(alloc_size >= request_size);
            }
        }
    }
}
UNITTEST_SUITE_END
