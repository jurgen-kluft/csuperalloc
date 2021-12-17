# TODO for xvmem

- separate superchunks_t, rename it to superregion_t, block_t to superblock_t.
- create a supermemory_t that holds superregion_t[], every superregion_t can
  have its own page size, memory protect setting and memory type attributes.
- multiple superallocator_t instances can use the same superregion_t instance.
- so we need a global configuration of supermemory_t that configures the
  supermemory_t and multiple superregion_t instances.

  

- Check => when allocating the size might be increased due to alignment
- Testing, Testing, Testing
- Benchmarks
- Multi-Threading
- Virtual Memory implementation for PS4 / Xbox One / ...
