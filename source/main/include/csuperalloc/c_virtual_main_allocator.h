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

    // A virtual memory allocator, suitable for CPU as well as GPU memory
    extern alloc_t* gCreateVmAllocator(alloc_t* main_heap, vmem_t* vmem

}; // namespace ncore

#endif // __C_ALLOCATOR_VIRTUAL_ALLOCATOR_H__