#include "cbase/c_allocator.h"
#include "cbase/c_integer.h"

#include "csuperalloc/c_superalloc.h"
#include "csuperalloc/test_allocator.h"
#include "cvmem/c_virtual_memory.h"

#include "cunittest/cunittest.h"

using namespace ncore;

class alloc_with_stats_t : public alloc_t
{
    valloc_t* mAllocator;
    u32     mNumAllocs;
    u32     mNumDeallocs;
    u64     mMemoryAllocated;
    u64     mMemoryDeallocated;

public:
    alloc_with_stats_t()
        : mAllocator(nullptr)
    {
        mNumAllocs         = 0;
        mNumDeallocs       = 0;
        mMemoryAllocated   = 0;
        mMemoryDeallocated = 0;
    }

    void init(alloc_t* allocator, vmem_t* vmem) 
    { 
        mAllocator = gCreateVmAllocator(allocator, vmem);
    }

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
        if (mAllocator == nullptr)
            return;
        gDestroyVmAllocator(mAllocator);
        mAllocator = nullptr;
    }
};

UNITTEST_SUITE_BEGIN(main_allocator)
{
    UNITTEST_FIXTURE(main)
    {
        UNITTEST_ALLOCATOR;

        UNITTEST_FIXTURE_SETUP()
        {
            vmem->initialize();
        }

        UNITTEST_FIXTURE_TEARDOWN()
		{
		}

        UNITTEST_TEST(init_release)
        {
            alloc_with_stats_t s_alloc;
            s_alloc.init(Allocator, vmem);
            s_alloc.release();
        }


        UNITTEST_TEST(init_alloc1_dealloc_release)
        {
            alloc_with_stats_t s_alloc;
            s_alloc.init(Allocator, vmem);

            void* ptr = s_alloc.allocate(10);
            s_alloc.deallocate(ptr);

            s_alloc.release();
        }
    }
}
UNITTEST_SUITE_END
