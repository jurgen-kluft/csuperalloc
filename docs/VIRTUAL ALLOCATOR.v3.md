# Virtual Allocator v3.0

Goal: more simplified logic.

Consideration: A region once setup cannot be teardown, also a chunk structure and its additional data when allocated cannot be deallocated anymore. In this way we can use an arena to allocate the meta-data and their size can be dynamic. When a chunk is suddenly empty we can free the associated virtual memory pages but not the meta-data, since the meta-data is locked to a certain allocation range, we should add this empty chunk to the appropriate free-list.

Note: Actually we can re-use the meta-data of chunk_t and block_t since a region has an array of indices to the chunks/blocks it contains.

- A region has a fixed size, for example 16 MB
- A chunked region is for small allocations
- A block-based region is for larger allocations

- A space can hold N regions
- A region can hold N chunks/blocks

Allocation sizes are computed based on a divisor (D is 4 or 8), and an allocation range is defined as (1 << N, 1 << (N+1)). There are thus D number of sizes in an allocation range.
Example: If D is 4, then an allocation range (64, 128) will have 4 allocation sizes: 64, 80, 96, 112.
A chunk is allocated for an allocation range, this means that the maximum number of allocations it can hold is determined by the size of the chunk and the smallest allocation size within that range, (chunk-size / (1 << N)). So the bitvec data should have enough bits to track all possible allocations within the chunk.

The allocator will have an array of list heads for different allocation ranges. Whenever a chunk has free elements, it will be added to the appropriate free-list based on its allocation range. When a chunk becomes completely full, it will be removed from the free-list.

The allocator also has an array of list heads to hold regions of particular sizes that still have unused chunks or blocks. This allows the allocator to quickly find regions with available ununsed chunks/blocks.

To allocate a new region, the allocator simply uses the address space and advances the pointer by the necessary size of the region, e.g. 16 MB, 32, 64, 128, 256 MB, etc.. 
A region size is a multiple of 16 MB, e.g. 16 MB << N, and it needs to fill (1 << N) items in the region array pointing to the same region, this is to make sure that a deallocation can find the correct region entry in the array.

Note: We make array's of u32[] reusable, this should make the reusability of chunks and regions a lot more likely.
      We make an array of power-of-two sized reusable arrays, which would allow us to efficiently manage and recycle memory for chunks and regions of different sizes.
      Although chunks need an array of u64 for m_o_bin1, we can look for a free array with (size << 1), since each u64 element can be represented by two u32 elements in the reusable array pool.
```c++
          struct array_t
          {
              u32 m_next;       // We can put a free array in a free list
              u32 m_capacity;   // The capacity of the array
              // u32 m_array[]; // The array of elements (in-memory after the struct)
          };
```


