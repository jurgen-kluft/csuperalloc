# A (virtual) memory allocator library

A library that provides cross-platform usage of a virtual memory allocator.

## Superalloc

Currently this allocator, called 'superalloc', is implemented in C++ and is around 1200 lines 
of code for the core.
This allocator is very configurable and all book-keeping data is outside of the managed memory
making it very suitable for different kind of memory (read-only, GPU etc..).

It only uses the following data structures:

* plain old c style arrays
* doubly linked list
* binmap; 3 layer bit array

```c++
class valloc_t
{
public:
      void* allocate(u32 size, u32 align);
      u32   deallocate(void*);
      
      // You can tag an allocation, very useful for attaching debugging info to an allocation or
      // using it as a CPU/GPU handle.
      void  set_tag(void*, u32);
      u32   get_tag(void*);
      u32   get_size(void*);
};
```

Note: Benchmarks are still to be done.  
Note: A large running test (60 million alloc/free operations) was done without crashing, so this 
      version is the first release candidate.

## WIP

Some things missing:

- cached chunks are not limited so nothing is released back in terms of unused physical pages. 

