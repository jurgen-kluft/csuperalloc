#include "xbase/x_allocator.h"
#include "xbase/x_integer.h"

#include "xunittest/xunittest.h"

#include "xvmem/private/x_binmap.h"

using namespace xcore;

extern alloc_t* gTestAllocator;

UNITTEST_SUITE_BEGIN(binmap)
{
    UNITTEST_FIXTURE(main)
    {
        UNITTEST_FIXTURE_SETUP() {}

        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(set_get) 
        {
            binmap_t bm;

            u16 l1[16];
            u16 l2[256];

            for (s32 i = 0; i < 16; ++i)
            {
                u32 count = 2050 + (i*41);
                bm.init(count, (u16*)&l1, 16, (u16*)&l2, 256);

                for (u32 b = 0; b < count; b += 2)
                {
                    bm.set(count, (u16*)&l1, (u16*)&l2, b);
                }
                for (u32 b = 0; b < count; b++)
                {
                    bool s = bm.get(count, (u16*)&l2, b);
                    CHECK_EQUAL(((b & 1) == 0), s);
                }
                for (u32 b = 1; b < count; b += 2)
                {
                    u32 f = bm.findandset(count, (u16*)&l1, (u16*)&l2);
                    CHECK_EQUAL(b, f);
                }

                // There should not be any more free places in the binmap
                for (u32 b = 1; b < 16; b += 2)
                {
                    s32 f = bm.find(count, (u16*)&l1, (u16*)&l2);
                    CHECK_EQUAL(-1, f);
                }
            }
        }
    }
}
UNITTEST_SUITE_END
