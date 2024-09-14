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

Each thread could have a thread proxy that will queue the free operation of any
non owning thread. A simple spin-lock can be used to synchronize access to the
hierarchical bitmap. We can have 2 bitmaps, when the owning thread is requested
to allocate memory, and if there are queued free operations, then it will swap 
the bitmaps and process the free operations. We could also make the processing
of the free operations explicit, so that the owning thread can process the free
operations at a time that is convenient from a user perspective.

