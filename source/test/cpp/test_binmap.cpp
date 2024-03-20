#include "ccore/c_allocator.h"
#include "cbase/c_integer.h"
#include "cbase/c_memory.h"

#include "csuperalloc/private/c_binmap.h"

#include "cunittest/cunittest.h"

using namespace ncore;

UNITTEST_SUITE_BEGIN(binmap)
{
    UNITTEST_FIXTURE(main)
    {
        UNITTEST_FIXTURE_SETUP() {}
        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(set_get)
        {
            u32 l0len, l1len, l2len, l3len;
            u32 const len = 32 * 32 * 32 * 32;
            binmap_t::compute_levels(len, l0len, l1len, l2len, l3len);

            u32* l1 = (u32*)TestAllocator->Allocate(sizeof(u32) * ((l1len+31) >> 5));
            u32* l2 = (u32*)TestAllocator->Allocate(sizeof(u32) * ((l2len+31) >> 5));
            u32* l3 = (u32*)TestAllocator->Allocate(sizeof(u32) * ((l3len+31) >> 5));
            nmem::memclr(l1, sizeof(u32) * ((l1len+31) >> 5));
            nmem::memclr(l2, sizeof(u32) * ((l2len+31) >> 5));
            nmem::memclr(l3, sizeof(u32) * ((l3len+31) >> 5));

            for (s32 i = 0; i < 64; ++i)
            {
                u32 const count = i*1024 + ((1 - (i&1)) * 600);
                binmap_t::compute_levels(count, l0len, l1len, l2len, l3len);
                binmap_t bm;
                bm.init_0(count, l0len, l1, l1len, l2, l2len, l3, l3len);

                for (u32 b = 0; b < count; b += 2)
                {
                    bm.set(b);
                }
                for (u32 b = 0; b < count; b++)
                {
                    bool const s = bm.get(b);
                    CHECK_EQUAL(((b & 1) == 0), s);
                }
                for (u32 b = 1; b < count; b += 2)
                {
                    u32 const f = bm.findandset();
                    CHECK_EQUAL(b, f);
                }

                // There should not be any more free places in the binmap
                s32 const f = bm.find();
                CHECK_EQUAL(-1, f);
            }

            TestAllocator->Deallocate(l1);
            TestAllocator->Deallocate(l2);
            TestAllocator->Deallocate(l3);
        }

        UNITTEST_TEST(lazy_init)
        {
            u32 l0len, l1len, l2len, l3len;
            u32 const len = 32 * 32 * 32 * 32;
            binmap_t::compute_levels(len, l0len, l1len, l2len, l3len);

            u32* l1 = (u32*)TestAllocator->Allocate(sizeof(u32) * ((l1len + 31) >> 5));
            u32* l2 = (u32*)TestAllocator->Allocate(sizeof(u32) * ((l2len + 31) >> 5));
            u32* l3 = (u32*)TestAllocator->Allocate(sizeof(u32) * ((l3len + 31) >> 5));
            nmem::memclr(l1, sizeof(u32) * ((l1len + 31) >> 5));
            nmem::memclr(l2, sizeof(u32) * ((l2len + 31) >> 5));
            nmem::memclr(l3, sizeof(u32) * ((l3len + 31) >> 5));

            for (u32 i = 0; i < 64; ++i)
            {
                u32 const count = i * 1024 + ((1 - (i & 1)) * 600);
                binmap_t::compute_levels(count, l0len, l1len, l2len, l3len);

                binmap_t bm;
                bm.init_lazy_0(count, l0len, l1, l1len, l2, l2len, l3, l3len);

                for (u32 b = 0; b < count; ++b)
                {
                    bm.lazy_init_0(b);
                }

                for (u32 b = 0; b < count; b += 2)
                {
                    CHECK_FALSE(bm.get(b));
                    bm.set(b);
                    CHECK_TRUE(bm.get(b));
                }
                for (u32 b = 0; b < count; b++)
                {
                    bool const s = bm.get(b);
                    CHECK_EQUAL(((b & 1) == 0), s);
                }

                for (u32 b = 1; b < count; b += 2)
                {
                    u32 const f = bm.findandset();
                    CHECK_EQUAL(b, f);
                }

                // There should not be any more free places in the binmap
                s32 const f = bm.find();
                CHECK_EQUAL(-1, f);
            }

            TestAllocator->Deallocate(l1);
            TestAllocator->Deallocate(l2);
            TestAllocator->Deallocate(l3);
        }
    }
}
UNITTEST_SUITE_END
