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

    namespace nsuperalloc
    {
        // A 'virtual memory' allocator, suitable for CPU as well as GPU memory
        // Note: This interface is not thread-safe
        // Note: It is almost possible to even derive from dexer_t, the u32 index would consist of
        //       - segment index (8 bits)
        //       - chunk index (15 bits)
        //       - item index (12 bits)
        //       If the allocator would cover (2^32 * smallest alloc size) memory then the index
        //       could fit into 32 bits.

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
    }  // namespace nsuperalloc

    // A 'virtual memory' allocator, suitable for CPU as well as GPU memory
    extern nsuperalloc::vmalloc_t* gCreateVmAllocator(alloc_t* main_heap);
    extern void                    gDestroyVmAllocator(nsuperalloc::vmalloc_t* allocator);

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
