# A (virtual) memory allocator library

A library that provides cross-platform usage of a virtual memory allocator.

Note: See [ccode](https://github.com/jurgen-kluft/ccode) on how to generate the buildfiles.

If you like my work and want to support me. Please consider to buy me a [coffee!](https://www.buymeacoffee.com/Jur93n)
<img src="bmacoffee.png" width="100">

## Superalloc

Currently this allocator, called 'superalloc', is implemented in C++ and is around 1200 lines 
of code for the core.
This allocator is very configurable and all book-keeping data is outside of the managed memory
making it very suitable for different types of memory (read-only, GPU etc..).

It only uses the following data structures:

* plain old c style arrays
* binmap; 4 layer bit array (maximum of 1 million (2^20) items)
* doubly linked list

Execution behaviour:

* Allocation is done in O(1) time
* Deallocation is done in O(1) time
* Set / Get tag is done in O(1) time

```c++
class vmalloc_t : public alloc_t
{
public:
      void* allocate(u32 size, u32 align);
      void  deallocate(void*);
      
      // You can tag an allocation, very useful for attaching debug info to an allocation or
      // using it as a CPU/GPU handle.
      void  set_tag(void*, u32);
      u32   get_tag(void*);
      u32   get_size(void*);
};
```

Note: Benchmarks are still to be done.  
Note: Unittest contains a test called `stress test` that executes 500.000 operations (allocation / deallocation)

## WIP

Some things missing:

- not multi-thread safe (wip)
- cached chunks are not limited so nothing is released back in terms of unused physical pages. 

