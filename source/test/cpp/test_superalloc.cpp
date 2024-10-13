#include "cbase/c_allocator.h"
#include "cbase/c_integer.h"

#include "csuperalloc/c_superalloc.h"
#include "cvmem/c_virtual_memory.h"

#include "cunittest/cunittest.h"

using namespace ncore;

extern unsigned char allocdmp[];
extern unsigned int  allocdmp_len;

class alloc_with_stats_t : public alloc_t
{
    nvmalloc::vmalloc_t* mAllocator;
    u32                  mNumAllocs;
    u32                  mNumDeallocs;
    u64                  mMemoryAllocated;
    u64                  mMemoryDeallocated;

public:
    alloc_with_stats_t()
        : mAllocator(nullptr)
    {
        mNumAllocs         = 0;
        mNumDeallocs       = 0;
        mMemoryAllocated   = 0;
        mMemoryDeallocated = 0;
    }

    void init(alloc_t* allocator) { mAllocator = gCreateVmAllocator(allocator); }

    void release(alloc_t* allocator)
    {
        gDestroyVmAllocator(mAllocator);
        mAllocator = nullptr;
    }

    virtual void* v_allocate(u32 size, u32 alignment)
    {
        mNumAllocs++;
        void* ptr = mAllocator->allocate(size, alignment);
        mMemoryAllocated += mAllocator->get_size(ptr);
        return ptr;
    }

    virtual void v_deallocate(void* ptr)
    {
        mNumDeallocs++;
        mMemoryDeallocated += mAllocator->get_size(ptr);
        mAllocator->deallocate(ptr);
    }

    inline u32  get_size(void* mem) const { return mAllocator->get_size(mem); }
    inline void set_tag(void* mem, u32 tag) { mAllocator->set_tag(mem, tag); }
    inline u32  get_tag(void* mem) const { return mAllocator->get_tag(mem); }
};

UNITTEST_SUITE_BEGIN(main_allocator)
{
    UNITTEST_FIXTURE(main)
    {
        UNITTEST_ALLOCATOR;

        UNITTEST_FIXTURE_SETUP()
        {
            // Initialize virtual memory
            ncore::nvmem::initialize();
        }

        UNITTEST_FIXTURE_TEARDOWN() {}

        UNITTEST_TEST(init_release)
        {
            alloc_with_stats_t s_alloc;
            s_alloc.init(Allocator);
            s_alloc.release(Allocator);
        }

        UNITTEST_TEST(init_alloc1_dealloc_release)
        {
            alloc_with_stats_t s_alloc;
            s_alloc.init(Allocator);

            void* ptr  = s_alloc.allocate(10);
            u32   size = s_alloc.get_size(ptr);
            CHECK_EQUAL(16, size);
            s_alloc.deallocate(ptr);
            s_alloc.release(Allocator);
        }

        UNITTEST_TEST(init_alloc_dealloc_10_release)
        {
            alloc_with_stats_t s_alloc;
            s_alloc.init(Allocator);

            for (s32 i = 0; i < 10; ++i)
            {
                void* ptr  = s_alloc.allocate(10);
                u32   size = s_alloc.get_size(ptr);
                CHECK_EQUAL(16, size);
                s_alloc.deallocate(ptr);
            }
            s_alloc.release(Allocator);
        }

        UNITTEST_TEST(init_alloc_10_dealloc_10_release)
        {
            alloc_with_stats_t s_alloc;
            s_alloc.init(Allocator);

            const s32 num_allocs = 10;
            void*     ptr[num_allocs];
            for (s32 i = 0; i < num_allocs; ++i)
            {
                ptr[i] = s_alloc.allocate(10);
            }
            for (s32 i = 0; i < num_allocs; ++i)
            {
                u32 size = s_alloc.get_size(ptr[i]);
                CHECK_EQUAL(16, size);
            }
            for (s32 i = 0; i < num_allocs; ++i)
            {
                s_alloc.deallocate(ptr[i]);
            }
            s_alloc.release(Allocator);
        }

        UNITTEST_TEST(init_alloc_tag_dealloc_release)
        {
            alloc_with_stats_t s_alloc;
            s_alloc.init(Allocator);

            void* ptr = s_alloc.allocate(10);
            s_alloc.set_tag(ptr, 0x12345678);
            u32 tag = s_alloc.get_tag(ptr);
            CHECK_EQUAL(0x12345678, tag);
            s_alloc.deallocate(ptr);

            s_alloc.release(Allocator);
        }

        UNITTEST_TEST(stress_test)
        {
            alloc_with_stats_t s_alloc;
            s_alloc.init(Allocator);

            s32*      allocations = (s32*)allocdmp;
            s64 const num_allocs  = allocdmp_len / sizeof(u32);

            s64 i = 0;
            while (i < num_allocs)
            {
                s32 size = allocations[i];
                if (size < 0)
                {
                    // this is a deallocation
                    size          = -size;
                    s64    offset = allocations[i + 1];
                    void** store  = (void**)(allocdmp + (offset * 8));
                    void*  ptr    = *store;
                    s_alloc.deallocate(ptr);
                }
                else
                {
                    s64*   ptr   = (s64*)s_alloc.allocate(size);
                    void** store = (void**)(allocdmp + (i * 4));
                    *store       = ptr;  // store the pointer of the allocation
                }
                i += 2;
            }

            s_alloc.release(Allocator);
        }
    }
}
UNITTEST_SUITE_END
