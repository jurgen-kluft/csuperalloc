#include "cbase/c_allocator.h"
#include "cbase/c_integer.h"

#include "cvmem/c_virtual_main_allocator.h"
#include "cvmem/c_virtual_memory.h"

#include "cunittest/cunittest.h"

using namespace ncore;

extern alloc_t* gTestAllocator;
class xalloc_with_stats : public alloc_t
{
    alloc_t* mAllocator;
    u32     mNumAllocs;
    u32     mNumDeallocs;
    u64     mMemoryAllocated;
    u64     mMemoryDeallocated;

public:
    xalloc_with_stats()
        : mAllocator(nullptr)
    {
        mNumAllocs         = 0;
        mNumDeallocs       = 0;
        mMemoryAllocated   = 0;
        mMemoryDeallocated = 0;
    }

    void init(alloc_t* allocator) { mAllocator = allocator; }

    virtual void* v_allocate(u32 size, u32 alignment)
    {
        mNumAllocs++;
        mMemoryAllocated += size;
        return mAllocator->allocate(size, alignment);
    }

    virtual u32 v_deallocate(void* mem)
    {
        mNumDeallocs++;
        u32 const size = mAllocator->deallocate(mem);
        mMemoryDeallocated += size;
        return size;
    }

    virtual void v_release()
    {
        mAllocator->release();
        mAllocator = nullptr;
    }
};

UNITTEST_SUITE_BEGIN(main_allocator)
{
    UNITTEST_FIXTURE(main)
    {
        xalloc_with_stats s_alloc;

        UNITTEST_FIXTURE_SETUP()
        {
            s_alloc.init(gTestAllocator);
        }

        UNITTEST_FIXTURE_TEARDOWN()
		{
		}

        UNITTEST_TEST(init)
        {
        }
    }
}
UNITTEST_SUITE_END
