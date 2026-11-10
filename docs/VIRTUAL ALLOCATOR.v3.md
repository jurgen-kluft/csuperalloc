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
A region size is a multiple of 16 MB, e.g. 16 MB << N, and it needs to fill (1<<N) items in the region array pointing to the same region, this is to make sure that a deallocation can find the correct region entry in the array.

```c++
        struct alloc_config_t
        {
            alloc_config_t(u32 alloc_size, u8 alloc_range, s8 chunk_sizeshift, s8 region_sizeshift, u8 region_type)
                : m_alloc_size(alloc_size)
                , m_alloc_range(alloc_range)
                , m_chunk_sizeshift(chunk_sizeshift)
                , m_region_sizeshift(region_sizeshift)
                , m_region_type(region_type)
            {
            }
            alloc_config_t(const alloc_config_t& other)
                : m_alloc_size(other.m_alloc_size)
                , m_alloc_range(other.m_alloc_range)
                , m_chunk_sizeshift(other.m_chunk_sizeshift)
                , m_region_sizeshift(other.m_region_sizeshift)
                , m_region_type(other.m_region_type)
            {
            }
            u32           m_alloc_size;       // The size of the allocation that this bin is managing
            u8            m_alloc_range;      // The allocation range index for this bin
            s8            m_chunk_sizeshift;  // The shift of the chunk/block size (e.g. 12 for 4KB)
            s8            m_region_sizeshift; // The shift of the region size (e.g. 24 for 16 MB)
            u8            m_region_type;      // The type of the region (e.g., chunk-based or block-based)
        };

        // A chunk based region consists of N chunks
        // A chunk consists of N elements with a maximum of 1024.
        // Note: Bitvector needs to be fully initialized, not lazily.
        struct chunk_t
        {
            u32 m_capacity : 12;   // max number of elements in the chunk
            u32 m_count : 12;      // cur number of elements in the chunk
            u32 m_alloc_index : 8; // allocation info index for this chunk
            u32 m_region_index;    // region this chunk belongs to (index into allocator->m_region_array)
            u32 m_onext;           // next chunk in list (offset into arena memory)
            u32 m_free_bin0;       // binmap, level 0 (5)
            //u32 m_free_bin1[];   // binmap, level 1 (5) (after this struct, max 128 bytes)
            //u32 m_tags[];        // tags for each element in the chunk (after the chunk struct + m_free_bin1[])
        };

        // A block based region consists of N blocks
        // A block is a fixed size number of pages (power-of-two number of pages).
        // But an allocation can use less pages than the block size, so we need to track
        // the number of committed pages per block.
        struct block_t
        {
            u32 m_pages : 24;       // number of committed pages in the block
            u32 m_alloc_index : 8;  // allocation info index for this block
            u32 m_region_index;     // region this block belongs to (index into allocator->m_region_array)
            u32 m_onext;            // next block in list (offset into arena memory)
            u32 m_tag;              // tag for the block
        };

        // A region consists of N chunks/blocks
        // Example size of a region: 16 MB
        // Chunks in a region are in the same size range
        // A region is thus dedicated to a specific alloc range
        struct region_t
        {
            u32 m_onext;             // next region in list (offset into arena memory)
            u32 m_index;             // index into allocator->m_region_array
            u8  m_region_type;       // the type of the region (e.g., chunk-based or block-based)
            u8  m_region_sizeshift;  // shift value to calculate the size of this region (1 << region_sizeshift)
            u16 m_free_slot_index;   // index of the first virgin chunk/block slot in m_array
            //u16 m_capacity;        // max num chunks/blocks (1 << (region_sizeshift - chunk/block_sizeshift))
            u16 m_count;             // current number of chunks/blocks in use
            u32 m_oarray;            // array of chunk/block offsets (offset into arena memory)
        };

        // An allocator consists of N regions
        // Example size of an allocator: 128 GB or 256 GB
        struct allocator_t
        {
            chunk_t*    m_active_chunks_per_alloc_index;
            chunk_t*    m_cached_empty_chunks_per_alloc_range;
            block_t*    m_cached_empty_blocks_per_alloc_range;
            chunk_t*    m_free_empty_chunks_per_alloc_range;
            block_t*    m_free_empty_blocks_per_alloc_range;
            region_t*   m_active_regions_per_alloc_range;

            alloc_info* m_alloc_info_array;

            s8          m_region_sizeshift;         // shift value to calculate region size
            
            byte*       m_memory_base;              // Virtual memory base for the allocator (aligned to region size)
            u64         m_memory_size;              // Size of the virtual memory for the allocator
            u32*        m_oregion_array;            // (nil = 0xffffffff), covers virtual memory range

            byte*       m_arena_memory_base;        // For allocating regions/chunks/blocks
            byte*       m_arena_memory_cursor;
            u64         m_arena_memory_size;        // Size of the arena memory for allocating regions/chunks/blocks
        };

```

