# Virtual Allocator v3.0

Goal: more simplified logic.

Consideration: A region once setup cannot be teardown, also a chunk structure and its additional data when allocated cannot be deallocated anymore. In this way we can use an arena to allocate the meta-data and their size can be dynamic. When a chunk is suddenly empty we can free the associated virtual memory pages but not the meta-data, since the meta-data is locked to a certain allocation range, we should add this empty chunk to the appropriate free-list.

Note: Actually we can re-use the meta-data of chunk_t and block_t since a region has an array of indices to the chunks/blocks it contains.

- A region has a fixed size, for example 16 MB
- A chunked region is for small allocations
- A block-based region is for larger allocations

- A space can hold N regions
- A region can hold N chunks/blocks

The allocator will have an array of list heads for different allocation ranges. Whenever a chunk has free elements, it will be added to the appropriate free-list based on its allocation range. When a chunk becomes completely full, it will be removed from the free-list.

The allocator also has an array of list heads to hold regions of particular sizes that still have unused chunks or blocks. This allows the allocator to quickly find regions with available ununsed chunks/blocks.

To allocate a new region, the allocator simply uses the address space and advances the pointer by the necessary size of the region, e.g. 16 MB, 32, 64, 128, 256 MB, etc.. 
A region size is a multiple of 16 MB, e.g. N * 16 MB, and it needs to fill N items in the region array pointing to the same region, this is to make sure that a deallocation can find the correct region entry in the array.

```c++
        // A chunk consists of N elements with a maximum of 1024.
        // Note: Bitvector needs to be fully initialized, not lazily.
        struct chunk_t
        {
            u32 m_capacity : 12;   // max number of elements in the chunk
            u32 m_count : 12;      // cur number of elements in the chunk
            u32 m_alloc_index : 8; // allocation info index for this chunk
            u32 m_region_index;    // region this chunk belongs to
            u32 m_next;            // next chunk in list
            u32 m_free_bin0;       // binmap, level 0 (5)
            //u32 m_free_bin1;     // binmap, level 1 (5) (fsa, idx2ptr, max 128 bytes)
        };

        // A region consists of N blocks
        // A block is a fixed size number of pages (power-of-two number of pages).
        // But an allocation can use less pages than the block size, so we need to track
        // the number of committed pages per block.
        struct block_t
        {
            u32 m_pages : 24;       // number of committed pages in the block
            u32 m_alloc_index : 8;  // allocation info index for this block
            u32 m_region_index;     // region this block belongs to
            u32 m_next;             // next block in list
        };

        // A region consists of N chunks/blocks
        // Example size of a region: 16 MB
        // Chunks in a region are of the same size (chunk_size_shift).
        // A region is dedicated to a specific index (alloc config).
        struct chunk_based_region_t
        {
            u16 m_chunk_free_index;  // index of the first free chunk/block in the region
            u16 m_chunk_capacity;    // maximum number of chunks/blocks in the region
            u16 m_chunk_count;       // current number of chunks/blocks in use
            u8  m_alloc_index;       // the alloc config index for this region
            u32 m_chunk_array;       // array of chunk/block indices (fsa, idx2ptr)
            u32 m_next;              // next region in list
        };

        struct block_based_region_t
        {
            u16 m_block_free_index;  // index of the first free block in the region
            u16 m_block_capacity;    // maximum number of blocks in the region
            u16 m_block_count;       // current number of blocks in use
            u8  m_alloc_index;       // the alloc config index for this region
            u32 m_block_array;       // array of block indices (fsa, idx2ptr)
            u32 m_next;              // next region in list
        };

        // An allocator consists of N regions
        // Example size of an allocator: 128 GB or 256 GB
        struct allocator_t
        {
            chunk_t* m_active_chunks_per_alloc_index;
            chunk_t* m_empty_chunks_per_alloc_range;
            block_t* m_empty_blocks_per_alloc_range;
            u8*      m_region_type;
            u32*     m_region_array;

            byte*    m_arena_memory_base;
            byte*    m_arena_memory_cursor;
        };

```

