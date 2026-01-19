# A (virtual) memory allocator library

A library that provides cross-platform usage of a virtual memory allocator.

Note: See [ccode](https://github.com/jurgen-kluft/ccode) on how to generate the buildfiles.

## Superalloc

Currently this allocator, called 'superalloc', is implemented in C++ and is around 1200 lines 
of code for the core.
This allocator is very configurable and all book-keeping data is outside of the managed memory
making it very suitable for different types of memory (read-only, GPU etc..).

It only uses the following data structures:

* array; plain old c style arrays
* list; doubly linked list
* binmap; N layer bit array

Execution behaviour:

* Allocation is done in O(1) time
* Deallocation is done in O(1) time
* Get size is done in O(1) time
* Set / Get tag is done in O(1) time

```c++
class vmalloc_t : public alloc_t
{
public:
    void* allocate(u32 size, u32 align);
    void  deallocate(void*);

    // Return the size that this allocation can use
    u32 get_size(void*) const; 
      
    // You can tag an allocation, very useful for attaching debug info 
    // to an allocation or using it as a CPU/GPU handle.
    void  set_tag(void*, u32);
    u32   get_tag(void*);
    u32   get_size(void*);
};
```

Note: Benchmarks are still to be done.  
Note: Unittest contains a test called `stress test` that executes 512K operations (allocation / deallocation)

## WIP

Some things missing:

- not multi-thread safe (wip)
- cached chunks are not limited so nothing is released back in terms of unused physical pages. 

