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

Not sure yet how to implement multi-threading, we could go into the direction
of having a superallocator_t per thread, however when pointers are shared 
between threads then we need to queue the free operation for the owning thread.

