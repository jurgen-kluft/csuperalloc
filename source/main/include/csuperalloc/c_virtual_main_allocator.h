#ifndef __C_ALLOCATOR_VIRTUAL_ALLOCATOR_H__
#define __C_ALLOCATOR_VIRTUAL_ALLOCATOR_H__
#include "ccore/c_target.h"
#ifdef USE_PRAGMA_ONCE
#pragma once
#endif

namespace ncore
{
    // Forward declares
    class alloc_t;
    class vmem_t;

    struct vmem_config_t
    {
        static inline u32 KB(u32 value) { return value * (u32)1024; }
        static inline u32 MB(u32 value) { return value * (u32)1024 * (u32)1024; }
        static inline u64 MBx(u64 value) { return value * (u64)1024 * (u64)1024; }
        static inline u64 GBx(u64 value) { return value * (u64)1024 * (u64)1024 * (u64)1024; }

        vmem_config_t()
        {
        }

    };

    // A virtual memory allocator, suitable for CPU as well as GPU memory
    extern alloc_t* gCreateVmAllocator(alloc_t* main_heap, vmem_t* vmem, vmem_config_t const* const cfg);

}; // namespace ncore

#endif // __C_ALLOCATOR_VIRTUAL_ALLOCATOR_H__