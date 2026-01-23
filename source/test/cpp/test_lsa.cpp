#include "cbase/c_allocator.h"
#include "cbase/c_integer.h"
#include "ccore/c_random.h"

#include "csuperalloc/c_lsa.h"

#include "cunittest/cunittest.h"

using namespace ncore;

UNITTEST_SUITE_BEGIN(lsa)
{
    UNITTEST_FIXTURE(main)
    {
        // UNITTEST_ALLOCATOR;

        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(init_release)
        {
            lsa_t* lsa = nlsa::new_lsa();
            nlsa::destroy(lsa);
        }

        UNITTEST_TEST(init_alloc1_dealloc_release)
        {
            lsa_t* lsa = nlsa::new_lsa(128 * cKB, 1024);

            void* ptr  = nlsa::allocate(lsa, 80 * cKB);
            u32   size = nlsa::get_size(lsa, ptr);
            CHECK_EQUAL((u32)80 * cKB, size);
            nlsa::deallocate(lsa, ptr);

            nlsa::destroy(lsa);
        }

        UNITTEST_TEST(init_alloc_dealloc_10_release)
        {
            lsa_t* lsa = nlsa::new_lsa();

            for (s32 i = 0; i < 10; ++i)
            {
                void* ptr  = nlsa::allocate(lsa, 52 * cKB);
                u32   size = nlsa::get_size(lsa, ptr);
                CHECK_EQUAL((u32)64 * cKB, size);
                nlsa::deallocate(lsa, ptr);
            }
            nlsa::destroy(lsa);
        }

        UNITTEST_TEST(init_alloc_10_dealloc_10_release)
        {
            lsa_t* lsa = nlsa::new_lsa(128 * cKB, 1024);

            const s32 num_allocs = 10;
            void*     ptr[num_allocs];
            for (s32 i = 0; i < num_allocs; ++i)
            {
                ptr[i] = nlsa::allocate(lsa, 70 * cKB);
            }
            for (s32 i = 0; i < num_allocs; ++i)
            {
                u32 size = nlsa::get_size(lsa, ptr[i]);
                CHECK_EQUAL((u32)80 * cKB, size);
            }
            for (s32 i = 0; i < num_allocs; ++i)
            {
                nlsa::deallocate(lsa, ptr[i]);
            }
            nlsa::destroy(lsa);
        }

        // Allocate and deallocate randomly many different sizes and lifetimes
        UNITTEST_TEST(stress_test)
        {
            const s32    num_allocs = 1024;
            const s32    max_size   = 1024;
            lsa_t*       lsa        = nlsa::new_lsa(1024 * cKB, 2048);
            void*        ptrs[num_allocs];
            u32          sizes[num_allocs];
            xor_random_t rand_gen;

            // Initialize
            for (s32 i = 0; i < num_allocs; ++i)
            {
                ptrs[i]  = nullptr;
                sizes[i] = 0;
            }

            // Allocate with random size
            for (s32 i = 0; i < num_allocs; ++i)
            {
                // Allocate
                sizes[i]        = g_random_s32_min_max(&rand_gen, 16, max_size);
                ptrs[i]         = nlsa::allocate(lsa, sizes[i] * cKB);
                u32 actual_size = nlsa::get_size(lsa, ptrs[i]);
                CHECK(actual_size >= sizes[i] * cKB);
            }

            i32 indices[num_allocs];
            for (s32 i = 0; i < num_allocs; ++i)
                indices[i] = i;

            // Shuffle
            for (s32 i = 0; i < num_allocs; ++i)
            {
                const s32 j    = g_random_s32_min_max(&rand_gen, 0, num_allocs);
                const i32 temp = indices[i];
                indices[i]     = indices[j];
                indices[j]     = temp;
            }

            // Deallocate in random order using the shuffled indices
            for (s32 i = 0; i < num_allocs; ++i)
            {
                const s32 index = indices[i];
                nlsa::deallocate(lsa, ptrs[index]);
                ptrs[index]  = nullptr;
                sizes[index] = 0;
            }

            nlsa::destroy(lsa);
        }
    }
}
UNITTEST_SUITE_END
