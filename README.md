# xcore virtual memory library (WIP)

A library that provides cross-platform usage of virtual memory.

## Superalloc

Currently this allocator is implemented, 'superalloc', that is ~1200 lines of code for the core.
This allocator is very configurable and all book-keeping data is outside of the managed memory
making it very suitable for different kind of memory (read-only, GPU etc..).
It only uses the following data structures:

* plain old c style arrays
* doubly linked list
* binmap; 3 layer bit array

```cpp
class alloc_t
{
      void* allocate(u32 size, u32 align) = 0;
      u32   deallocate(void*) = 0;
      u32   get_size() = 0;
      
      void  set_tag(void*, u32) = 0;
      u32   get_tag(void*) = 0;
};
```

Note: Benchmarks are still to be done.  
Note: A large running test (60 million alloc/free operations) was done without crashing, so this 
      version is the first release candidate.

## WIP

Some things are missing though, cached chunks are not limited so nothing is released back in terms
of unused physical pages. Also adding support for tagging allocations with a 32-bit integer, usefull
for adding debugging support or GPU pointer mapping.

