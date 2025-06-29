# TODO for xvmem

- Investigate the use of madvise(MADV_FREE) to decommit memory on Mac, madvise(MADV_DONTNEED) on Linux, and VirtualAlloc(MEM_RESET).
  Especially for segments, sections and chunks.
- Create a supermemory_t that holds superregion_t[], every superregion_t can
  have its own page size, memory protect setting and memory type attributes.
- Multiple superallocator_t instances can use the same superregion_t instance.
- We need a global configuration of supermemory_t that configures the
  supermemory_t and multiple superregion_t instances.
- Testing, Testing, Testing
- Benchmarks
- Virtual Memory implementation for PS4 / Xbox One / ...


## Multi-Threading

TBD


