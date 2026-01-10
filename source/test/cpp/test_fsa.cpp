#include "cbase/c_allocator.h"
#include "cbase/c_integer.h"
#include "ccore/c_random.h"

#include "csuperalloc/c_fsa.h"

#include "cunittest/cunittest.h"

using namespace ncore;

UNITTEST_SUITE_BEGIN(fsa)
{
    UNITTEST_FIXTURE(main)
    {
        // UNITTEST_ALLOCATOR;

        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(init_release)
        {
            fsa_t* fsa = nfsa::new_fsa();
            nfsa::destroy(fsa);
        }

        UNITTEST_TEST(init_alloc1_dealloc_release)
        {
            fsa_t* fsa = nfsa::new_fsa();

            void* ptr  = nfsa::allocate(fsa, 1);
            u32   size = nfsa::get_size(fsa, ptr);
            CHECK_EQUAL((u32)8, size);
            nfsa::deallocate(fsa, ptr);

            nfsa::destroy(fsa);
        }

        UNITTEST_TEST(init_alloc_dealloc_10_release)
        {
            fsa_t* fsa = nfsa::new_fsa();

            for (s32 i = 0; i < 10; ++i)
            {
                void* ptr  = nfsa::allocate(fsa, 12);
                u32   size = nfsa::get_size(fsa, ptr);
                CHECK_EQUAL((u32)16, size);
                nfsa::deallocate(fsa, ptr);
            }
            nfsa::destroy(fsa);
        }

        UNITTEST_TEST(init_alloc_10_dealloc_10_release)
        {
            fsa_t* fsa = nfsa::new_fsa();

            const s32 num_allocs = 10;
            void*     ptr[num_allocs];
            for (s32 i = 0; i < num_allocs; ++i)
            {
                ptr[i] = nfsa::allocate(fsa, 10);
            }
            for (s32 i = 0; i < num_allocs; ++i)
            {
                u32 size = nfsa::get_size(fsa, ptr[i]);
                CHECK_EQUAL((u32)16, size);
            }
            for (s32 i = 0; i < num_allocs; ++i)
            {
                nfsa::deallocate(fsa, ptr[i]);
            }
            nfsa::destroy(fsa);
        }

        // Allocate and deallocate randomly many different sizes and lifetimes
        UNITTEST_TEST(stress_test)
        {
            const s32    num_allocs = 8192;
            const s32    max_size   = 1024;
            fsa_t*       fsa        = nfsa::new_fsa();
            void*        ptrs[num_allocs];
            u32          sizes[num_allocs];
            xor_random_t rand_gen;

            // Initialize
            for (s32 i = 0; i < num_allocs; ++i)
            {
                ptrs[i]  = nullptr;
                sizes[i] = 0;
            }

            // Randomly allocate and deallocate
            for (s32 iter = 0; iter < 10000; ++iter)
            {
                s32 index = g_random_s32_0_max(&rand_gen, num_allocs - 1);
                if (ptrs[index] == nullptr)
                {
                    // Allocate
                    sizes[index] = g_random_s32_min_max(&rand_gen, 8, max_size);
                    ptrs[index]  = nfsa::allocate(fsa, sizes[index]);
                    u32 actual_size = nfsa::get_size(fsa, ptrs[index]);
                    CHECK(actual_size >= sizes[index]);
                }
                else
                {
                    // Deallocate
                    nfsa::deallocate(fsa, ptrs[index]);
                    ptrs[index]  = nullptr;
                    sizes[index] = 0;
                }
            }

            // Cleanup
            for (s32 i = 0; i < num_allocs; ++i)
            {
                if (ptrs[i] != nullptr)
                {
                    nfsa::deallocate(fsa, ptrs[i]);
                    ptrs[i]  = nullptr;
                    sizes[i] = 0;
                }
            }

            nfsa::destroy(fsa);
        }
    }
}
UNITTEST_SUITE_END
