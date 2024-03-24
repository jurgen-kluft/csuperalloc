#ifndef __C_SUPERALLOC_ALLOCATOR_H__
#define __C_SUPERALLOC_ALLOCATOR_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#    pragma once
#endif

#include "cbase/c_allocator.h"

namespace ncore
{
    // Forward declares
    class vmem_t;

    class valloc_t : public alloc_t
    {
    public:
        inline u32  get_size(void* ptr) const { return v_get_size(ptr); }
        inline void set_tag(void* ptr, u32 assoc) { return v_set_tag(ptr, assoc); }
        inline u32  get_tag(void* ptr) const { return v_get_tag(ptr); }

    protected:
        virtual u32  v_get_size(void* ptr) const     = 0;
        virtual void v_set_tag(void* ptr, u32 assoc) = 0;
        virtual u32  v_get_tag(void* ptr) const      = 0;
    };

    // A 'virtual memory' allocator, suitable for CPU as well as GPU memory
    extern valloc_t* gCreateVmAllocator(alloc_t* main_heap);
    extern void gDestroyVmAllocator(valloc_t* allocator);

};  // namespace ncore

#endif