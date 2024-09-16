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

    namespace nvmalloc
    {
        // A 'virtual memory' allocator, suitable for CPU as well as GPU memory
        // Note: This interface is not thread-safe
        class vmalloc_t : public alloc_t
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
    }  // namespace nvmalloc

    // A 'virtual memory' allocator, suitable for CPU as well as GPU memory
    extern nvmalloc::vmalloc_t* gCreateVmAllocator(alloc_t* main_heap);
    extern void                 gDestroyVmAllocator(nvmalloc::vmalloc_t* allocator);

    // --------------------------------------------------------------------------------------------
    // A 'virtual memory' allocator, multi-thread safe
    struct vm_allocator_threaded_context_t;
    struct vm_allocator_threaded_t;

    extern vm_allocator_threaded_context_t* gCreateVmAllocatorThreadedContext(alloc_t* main_heap);
    extern vm_allocator_threaded_t*         gCreateVmAllocatorForThread(vm_allocator_threaded_context_t* ctxt, s16 thread_index);
    extern void                             gFinalizeVmAllocatorForThread(vm_allocator_threaded_context_t* ctxt, vm_allocator_threaded_t* allocator);

    extern void gVmAllocatorCollectDeferred(vm_allocator_threaded_t* ctxt);
};  // namespace ncore

#endif
