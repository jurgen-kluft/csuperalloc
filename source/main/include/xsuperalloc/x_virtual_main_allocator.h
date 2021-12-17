#ifndef __X_ALLOCATOR_VIRTUAL_ALLOCATOR_H__
#define __X_ALLOCATOR_VIRTUAL_ALLOCATOR_H__
#include "xbase/x_target.h"
#ifdef USE_PRAGMA_ONCE
#pragma once
#endif

namespace xcore
{
    // Forward declares
    class alloc_t;
    class xvmem;

    struct xvmem_config
    {
        static inline u32 KB(u32 value) { return value * (u32)1024; }
        static inline u32 MB(u32 value) { return value * (u32)1024 * (u32)1024; }
        static inline u64 MBx(u64 value) { return value * (u64)1024 * (u64)1024; }
        static inline u64 GBx(u64 value) { return value * (u64)1024 * (u64)1024 * (u64)1024; }

        xvmem_config()
        {
        }

    };

    // A virtual memory allocator, suitable for CPU as well as GPU memory
    extern alloc_t* gCreateVmAllocator(alloc_t* main_heap, xvmem* vmem, xvmem_config const* const cfg);

}; // namespace xcore

#endif // __X_ALLOCATOR_VIRTUAL_ALLOCATOR_H__