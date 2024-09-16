# TODO for xvmem

- Create a supermemory_t that holds superregion_t[], every superregion_t can
  have its own page size, memory protect setting and memory type attributes.
- Multiple superallocator_t instances can use the same superregion_t instance.
- We need a global configuration of supermemory_t that configures the
  supermemory_t and multiple superregion_t instances.
- Testing, Testing, Testing
- Benchmarks
- Virtual Memory implementation for PS4 / Xbox One / ...


## Multi-Threading

`nsuperspace` will be shared among the threads, but `nsuperheap` and `nsuperfsa` will
have an instance per thread.
So we extract `nsuperheap` and `nsuperfsa` from `superallocator` and make them dedicated 
per thread. This means that we have a certain instance per thread that is used for
allocating and deallocating memory. When allocating the instance is using `nsuperspace` to
request a `segment`, and a `segment` is used




