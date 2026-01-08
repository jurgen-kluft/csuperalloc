#include "cbase/c_allocator.h"
#include "cbase/c_integer.h"

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
            CHECK_EQUAL((u32)16, size);
            nfsa::deallocate(fsa, ptr);

            nfsa::destroy(fsa);
        }

        UNITTEST_TEST(init_alloc_dealloc_10_release)
        {
            fsa_t* fsa = nfsa::new_fsa();

            for (s32 i = 0; i < 10; ++i)
            {
                void* ptr  = nfsa::allocate(fsa, 16);
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
    }
}
UNITTEST_SUITE_END
